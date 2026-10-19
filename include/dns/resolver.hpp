#pragma once

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
#include "dns/error.hpp" // IWYU pragma: export
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

    // lookup_function is a blocking lookup of a host's addresses, as the system back end's getaddrinfo() is.
    using lookup_function =
        std::function<std::expected<std::vector<ip_addr>, std::error_code>(const std::string& host, protocol proto)>;

    struct resolver_state;

}

// resolver looks up host names' addresses, with a back end: for now, the system's resolver (getaddrinfo()), on the
// scheduler's threads for blocking calls (see io::scheduler::run_blocking()).
//
// Concurrent calls for the same name (ignoring case) and families, on the same scheduler, share one lookup (as Go's
// singleflight merges them), each caller with its own deadline and stop. Nothing is cached: a call after a lookup has
// finished starts another.
//
// A caller gives up at its deadline or when stop is requested (or the scheduler is stopped), at once, whatever the back
// end is doing, and the others sharing its lookup go on waiting. Once they all have, the lookup is stopped, and a later
// call starts another rather than join it: as getaddrinfo() can't be interrupted, it goes on, holding its thread until
// it returns, and its result is dropped. (A lookup whose callers have all given up before it got a thread doesn't
// start.) The scheduler's shutdown() waits for it, as for any task.
//
// Copies share their state, and lookups keep it, so a resolver may go before the lookups it started.
class resolver
{
public:
    enum class backend : std::uint8_t
    {
        // the best for the platform: for now, always the system's.
        automatic,
        // the system's resolver, getaddrinfo().
        system,
    };

    struct options
    {
        backend use = backend::automatic;

        // Internal (for tests): replaces getaddrinfo() for the system back end, if set. It's called on one of the
        // scheduler's threads for blocking calls, and mustn't throw.
        detail::lookup_function system_lookup{};
    };

    resolver();
    explicit resolver(options opts);

    // resolve looks up host's addresses (with options.proto's families), on scheduler, in the order the back end gives
    // them (getaddrinfo() sorts them by RFC 6724). An IP address (in its usual form, e.g. "127.0.0.1" or "::1") is
    // returned without a lookup (no thread, no query, nothing to wait for). It fails with:
    // - std::errc::invalid_argument if host isn't a valid name: labels of 1 to 63 letters, digits, '-' (not first or
    //   last) and '_', joined by dots, 253 bytes at most (not counting a final dot). (No IDNA, and no IPv6 zones,
    //   which ip_addr doesn't have.)
    // - std::errc::address_family_not_supported if host is an IP address that options.proto rules out.
    // - std::errc::timed_out if options.deadline passes first.
    // - std::errc::operation_canceled if options.stop is requested, or the scheduler is stopped (or was).
    // - otherwise, the back end's error: compare it with condition (e.g. condition::not_found).
    //
    // NOTE: not a coroutine itself: host and options are taken before it returns, and the resolver needn't outlive
    //       the task.
    coro::task<std::expected<std::vector<ip_addr>, std::error_code>>
    resolve(io::scheduler* scheduler, std::string host, resolve_options options = {}) const;

private:
    std::shared_ptr<detail::resolver_state> state;
};

// default_resolver is the process's resolver (as Go's net.DefaultResolver): what tcp_socket::connect() and the HTTP
// client use unless they're given another.
[[nodiscard]] resolver& default_resolver();

}
