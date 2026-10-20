#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <vector>

#include "coro/task.hpp"
#include "dns/config.hpp" // IWYU pragma: export
#include "dns/error.hpp"  // IWYU pragma: export
#include "io/scheduler.hpp"
#include "ip_addr.hpp"

namespace net::dns
{

// resolve_options are what a resolver::resolve() call may be given.
struct resolve_options
{
    // Which addresses to look up: both families', or only IPv4's or IPv6's.
    protocol proto = protocol::not_care;

    // When to give up (failing with std::errc::timed_out). None: the back end's own limits (e.g. getaddrinfo()'s).
    std::optional<io::clock::time_point> deadline{};

    // Requesting stop gives up (failing with std::errc::operation_canceled).
    std::stop_token stop{};
};

namespace detail
{

    // lookup_function is a blocking lookup of a host's addresses, as the system back end's getaddrinfo() is. A lookup
    // of both families calls it once for each (protocol::ipv6 and protocol::ipv4), in parallel.
    using lookup_function =
        std::function<std::expected<std::vector<ip_addr>, std::error_code>(const std::string& host, protocol proto)>;

    // system_facts are what resolver::backend::automatic reads of the machine besides the configuration files.
    // (Internal: tests replace them.)
    struct system_facts
    {
        // whether the OS is one whose configuration the native back end reads (Linux and the BSDs but OpenBSD). None:
        // this one's.
        std::optional<bool> native_os{};

        // reads an environment variable (none: it isn't set). Empty: std::getenv().
        std::function<std::optional<std::string>(const char* name)> getenv{};

        // the machine's name, or empty if it can't be had. Empty: gethostname().
        std::function<std::string()> host_name{};
    };

    struct resolver_state;
    struct lookup;

    // found is what a lookup has found so far (see resolution::look()).
    struct found
    {
        // once done, the lookup's addresses (as resolve() returns them), or err (none: why). Until then, the addresses
        // of the first family to have any (family's), which came in at `at`; or none, if neither has yet.
        bool                  done = false;
        std::vector<ip_addr>  addrs{};
        std::error_code       err{};
        protocol              family = protocol::not_care;
        io::clock::time_point at{};

        // how many times the lookup had told its callers of something new: what resolution::news() holds until the
        // next time.
        std::uint64_t version = 0;
    };

    // resolution is a caller's share of a lookup, for tcp_socket::connect(), which starts connecting on the first
    // family's addresses (RFC 8305 section 3) rather than waiting for both: the lookup tells it of each as it comes
    // in (see resolver::start_resolving()). It leaves the lookup when it's destroyed (as resolve()'s callers do when
    // they give up), unless the lookup is done. Not thread-safe (but the lookup it shares is).
    class resolution
    {
    public:
        // a resolution that's done: e.g. host was an IP address.
        explicit resolution(std::vector<ip_addr> addrs) noexcept;
        resolution(std::shared_ptr<resolver_state> resolver, std::shared_ptr<lookup> state) noexcept;

        resolution(const resolution&)            = delete;
        resolution& operator=(const resolution&) = delete;
        resolution(resolution&& other) noexcept;
        resolution& operator=(resolution&&) = delete;
        ~resolution();

        // look is what the lookup has found so far.
        [[nodiscard]] found look() const;

        // news is bumped (and notified, with the lookup's scheduler's notify_all()) each time the lookup has found
        // something new, or is done: to wait for, with found::version as what it holds now.
        [[nodiscard]] const std::atomic<std::uint64_t>& news() const noexcept;

        // watch has word bumped and notified too (once) the next time news is, unless it no longer holds version: then
        // it returns false, and there's something new to look at. (For a caller that waits on a word of its own, for
        // other things too.) It throws if it can't allocate.
        bool watch(std::shared_ptr<std::atomic<std::uint64_t>> word, std::uint64_t version);

    private:
        std::shared_ptr<resolver_state> resolver;
        std::shared_ptr<lookup>         state; // null: done at once, with addrs
        std::vector<ip_addr>            addrs;

        // the word watch() was last given, which leaving takes back.
        std::atomic<std::uint64_t>* watching = nullptr;
    };

}

// resolver looks up host names' addresses, with a back end: the system's resolver (getaddrinfo()), on the scheduler's
// threads for blocking calls (see io::scheduler::run_blocking()), or its own (native), which sends DNS queries itself,
// as Go's resolver does.
//
// Concurrent calls for the same name (ignoring case) and families, on the same scheduler, with the same back end (as
// automatic chooses it), share one lookup (as Go's singleflight merges them), each caller with its own deadline and
// stop. Nothing is cached: a call after a lookup has finished starts another.
//
// A caller gives up at its deadline or when stop is requested (or the scheduler is stopped), at once, whatever the back
// end is doing, and the others sharing its lookup go on waiting. Once they all have, the lookup is stopped, and a later
// call starts another rather than join it. The native back end's queries end then; as getaddrinfo() can't be
// interrupted, the system's goes on, holding its thread until it returns, and its result is dropped. (A lookup whose
// callers have all given up before it got a thread doesn't start.) The scheduler's shutdown() waits for it, as for any
// task.
//
// Copies share their state, and lookups keep it, so a resolver may go before the lookups it started.
class resolver
{
public:
    enum class backend : std::uint8_t
    {
        // chosen per name, as Go chooses (see docs/guide.md): the native on Linux and the BSDs (but OpenBSD), unless
        // the configuration has something it doesn't do (an option resolv.conf has that it doesn't know; LOCALDOMAIN
        // set, or RES_OPTIONS or HOSTALIASES not empty, in the environment; in nsswitch.conf's hosts line, a source
        // besides files and dns, or an action besides the defaults), or the name is one that such a source answers
        // (a .local name, with an mdns source; the machine's own name, or localhost, with myhostname: other names skip
        // them); otherwise, and on other OSes (macOS's configuration isn't all in /etc), the system's. The
        // configuration is the native back end's (conf, or the files', read again as they change), so the choice
        // follows it. NET_DNS=system or NET_DNS=native in the environment (read when the resolver is made; other
        // values are ignored) makes it that one.
        automatic,
        // the system's resolver, getaddrinfo(): for both families, a call for each (AF_INET6 and AF_INET), in
        // parallel, on threads of their own, their addresses then sorted by RFC 6724 together (as one call's would be).
        // One family failing (e.g. a name without AAAA records, or AI_ADDRCONFIG on a host without IPv6) isn't an
        // error if the other has addresses; if both fail, the error is a temporary failure's (as condition has it),
        // if only one is one, and otherwise IPv4's (IPv6's failure being the likelier to be the host's own, with
        // AI_ADDRCONFIG).
        system,
        // the resolver's own, as Go's: it reads the configuration (see config), looks names up in the hosts table, and
        // asks the nameservers itself, over UDP from a new socket (so a new random source port) each attempt, with
        // random IDs and EDNS(0) (again without it, if a server answers FORMERR), over TCP if an answer's truncated,
        // within the configuration's timeout per attempt, attempts and search list; it follows CNAME records within
        // an answer, and sorts the addresses by RFC 6724. Its errors are errc's.
        native,
    };

    struct options
    {
        backend use = backend::automatic;

        // the native back end's configuration, instead of reading /etc/resolv.conf, /etc/hosts and
        // /etc/nsswitch.conf (again if they change, at most every 5 seconds).
        std::optional<config> conf{};

        // Internal (for tests): replaces getaddrinfo() for the system back end, if set. It's called on one of the
        // scheduler's threads for blocking calls, and mustn't throw.
        detail::lookup_function system_lookup{};

        // Internal (for tests): the files the native back end's configuration is read from, if conf isn't set.
        detail::config_files files{};

        // Internal (for tests): what automatic reads of the machine, besides the configuration.
        detail::system_facts facts{};
    };

    resolver();
    explicit resolver(options opts);

    // resolve looks up host's addresses (with options.proto's families), on scheduler, in the order the back end gives
    // them (sorted by RFC 6724, as getaddrinfo() sorts them too; but the hosts table's, in its order). An IP address
    // (in its usual form, e.g. "127.0.0.1" or "::1") is returned without a lookup (no thread, no query, nothing to wait
    // for). It fails with:
    // - std::errc::invalid_argument if host isn't a valid name: labels of 1 to 63 letters, digits, '-' (not first or
    //   last) and '_', joined by dots, 253 bytes at most (not counting a final dot). (No IDNA, and no IPv6 zones,
    //   which ip_addr doesn't have.)
    // - std::errc::address_family_not_supported if host is an IP address that options.proto rules out.
    // - std::errc::timed_out if options.deadline passes first.
    // - std::errc::operation_canceled if options.stop is requested, or the scheduler is stopped (or was).
    // - otherwise, the back end's error: compare it with condition (e.g. condition::not_found). The native back end's
    //   are errc's: name_not_found (NXDOMAIN, for every name the search list made), no_address (the name has no
    //   addresses of the families asked for), server_failure (no server answered, or they failed), refused, or
    //   bad_response (e.g. a CNAME chain that loops). Without a deadline, it gives up once it has made its attempts.
    //
    // NOTE: not a coroutine itself: host and options are taken before it returns, and the resolver needn't outlive
    //       the task.
    coro::task<std::expected<std::vector<ip_addr>, std::error_code>>
    resolve(io::scheduler* scheduler, std::string host, resolve_options options = {}) const;

    // Internal (for tcp_socket::connect()): start_resolving starts looking host up (with proto's families), or joins
    // the lookup in flight, as resolve() does, but returns at once: the resolution tells of each family's addresses as
    // they come in (the system back end looks each up on its own thread; the native tells of a family as soon as a
    // server has answered with its addresses, if the other's question is still open), and of the lookup's result
    // once it's done. An IP address is a resolution that's done. It fails as resolve() does without waiting
    // (std::errc::invalid_argument, address_family_not_supported).
    [[nodiscard]] std::expected<detail::resolution, std::error_code>
    start_resolving(io::scheduler* scheduler, std::string host, protocol proto) const;

private:
    std::shared_ptr<detail::resolver_state> state;
};

// default_resolver is the process's resolver (as Go's net.DefaultResolver): what tcp_socket::connect() and the HTTP
// client use unless they're given another.
[[nodiscard]] resolver& default_resolver();

}
