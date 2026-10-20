#include "dns/resolver.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>

#include "coro/task.hpp"
#include "dns/automatic.hpp"
#include "dns/config.hpp"
#include "dns/native.hpp"
#include "exception.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "sockaddr.hpp"
#include "util/defer.hpp"
#include "util/hash.hpp"
#include "util/string_util.hpp"

namespace
{

using lookup_result = std::expected<std::vector<net::ip_addr>, std::error_code>;

// literal is host's address, if it's an IP address in its usual form.
//
// An IPv4 address must be in its usual form (as to_string() gives it): getaddrinfo() also takes others, which it reads
// differently (e.g. "010.0.0.1" as octal, 8.0.0.1, where ip_addr::parse() reads 10.0.0.1, and "127.1" as 127.0.0.1),
// so they're left to it.
std::optional<net::ip_addr> literal(const std::string& host) noexcept
{
    auto addr = net::ip_addr::parse(host);
    if (!addr.has_value()) return std::nullopt;

    try
    {
        if (addr->is_ipv4() && addr->to_string() != host) return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt; // to_string() can only fail to allocate: getaddrinfo() decides.
    }

    return *addr;
}

bool of_protocol(const net::ip_addr& addr, net::protocol proto) noexcept
{
    switch (proto)
    {
    case net::protocol::not_care: return true;
    case net::protocol::ipv4: return addr.is_ipv4();
    case net::protocol::ipv6: return addr.is_ipv6();
    }

    return false;
}

// valid_name is whether name is a domain name that may be looked up, as Go's isDomainName has it: labels of 1 to 63
// letters, digits, '-' and '_' ('_' being in real names, e.g. SRV records', and '-' neither first nor last), joined by
// dots, at most 253 bytes, not counting a final dot. All-numeric names (e.g. "127.1") are allowed, unlike Go's: the
// system's resolver reads them as addresses.
bool valid_name(std::string_view name) noexcept
{
    constexpr std::size_t max_name  = 253;
    constexpr std::size_t max_label = 63;

    if (name.ends_with('.')) name.remove_suffix(1);
    if (name.empty() || name.size() > max_name) return false;

    char        last  = '.';
    std::size_t label = 0;

    for (const char c : name)
    {
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit  = c >= '0' && c <= '9';

        if (letter || digit || c == '_')
        {
            ++label;
        }
        else if (c == '-')
        {
            if (last == '.') return false;
            ++label;
        }
        else if (c == '.')
        {
            if (last == '.' || last == '-' || label > max_label) return false;
            label = 0;
        }
        else
        {
            return false;
        }

        last = c;
    }

    return last != '-' && label <= max_label;
}

int family_hint(net::protocol proto) noexcept
{
    switch (proto)
    {
    case net::protocol::not_care: return AF_UNSPEC;
    case net::protocol::ipv4: return AF_INET;
    case net::protocol::ipv6: return AF_INET6;
    }
    return AF_UNSPEC;
}

// getaddrinfo_lookup looks host up with getaddrinfo(), blocking until it's done: proto's addresses, but a numeric host
// (in a form literal() leaves to getaddrinfo(), e.g. "127.1") read as one of numeric's, so that a lookup of each family
// on its own reads it as the address it is, rather than asking DNS for the other family's records of it as a name.
lookup_result getaddrinfo_lookup(const std::string& host, net::protocol proto, net::protocol numeric) noexcept
{
    // NOTE: not a designated initializer, as addrinfo's fields are in a different order on each platform.
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM; // one of each address, rather than one for each kind of socket.

    // An address is read as one. A name only resolves to the kinds of address this host has (AI_ADDRCONFIG), but that
    // would also reject an IPv6 address when there's no IPv6 network configured (glibc doesn't count loopback
    // addresses).
    addrinfo* found = nullptr;

    hints.ai_family = family_hint(numeric);
    hints.ai_flags  = AI_NUMERICHOST;
    auto err        = ::getaddrinfo(host.c_str(), nullptr, &hints, &found);
    if (err == EAI_NONAME)
    {
        hints.ai_family = family_hint(proto);
        hints.ai_flags  = AI_ADDRCONFIG;
        err             = ::getaddrinfo(host.c_str(), nullptr, &hints, &found);
    }

    if (err != 0) return std::unexpected(net::make_gai_error_code(err));

    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> owned{found, &::freeaddrinfo};

    try
    {
        std::vector<net::ip_addr> addrs;
        for (const auto* info = found; info != nullptr; info = info->ai_next)
        {
            // NOTE: an IPv6 address's zone is dropped (ip_addr has none), so two may become the same.
            auto addr = net::detail::from_sockaddr(info->ai_addr, info->ai_addrlen);
            if (addr.has_value() && std::ranges::find(addrs, addr->addr) == addrs.end()) addrs.push_back(addr->addr);
        }

        if (addrs.empty()) return std::unexpected(net::make_gai_error_code(EAI_NONAME));
        return addrs;
    }
    catch (...) // failing to allocate (or, in theory, a vector too long)
    {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

// at_once is host's result if it needn't be looked up (as resolve() describes): an IP address (or one of a family proto
// rules out), or a name that isn't valid.
std::optional<lookup_result> at_once(const std::string& host, net::protocol proto)
{
    if (auto addr = literal(host); addr.has_value())
    {
        if (!of_protocol(*addr, proto))
            return lookup_result{std::unexpected(std::make_error_code(std::errc::address_family_not_supported))};
        return lookup_result{std::vector{*addr}};
    }

    if (!valid_name(host)) return lookup_result{std::unexpected(std::make_error_code(std::errc::invalid_argument))};
    return std::nullopt;
}

using backend = net::dns::resolver::backend;

// lookup_key is what makes two lookups the same, so concurrent callers share one: the scheduler (whose word the
// callers wait on), the name (lower-cased: DNS names are case-insensitive), the families, and the back end (which
// automatic chooses per name, and again as the configuration changes).
struct lookup_key
{
    net::io::scheduler* scheduler = nullptr;
    std::string_view    host{};
    net::protocol       proto = net::protocol::not_care;
    backend             use   = backend::system;

    bool operator==(const lookup_key&) const = default;
};

struct lookup_key_hash
{
    std::size_t operator()(const lookup_key& key) const noexcept
    {
        auto hash = net::util::detail::hash_combine(0, key.host);
        hash      = net::util::detail::hash_combine(hash, key.scheduler);
        hash      = net::util::detail::hash_combine(hash, key.proto);
        return net::util::detail::hash_combine(hash, key.use);
    }
};

using watcher = std::shared_ptr<std::atomic<std::uint64_t>>;

}

namespace net::dns
{

namespace detail
{

    // lookup is a lookup in flight: what it looks up, what it has found so far, its result once it has one, and its
    // callers.
    struct lookup
    {
        lookup(io::scheduler* scheduler, std::string host, protocol proto, backend use) noexcept
            : scheduler{scheduler}
            , host{std::move(host)}
            , proto{proto}
            , use{use}
        {}

        io::scheduler* const scheduler;
        const std::string    host; // lower-cased
        const protocol       proto;
        const backend        use; // system or native

        // done is 1 once result is set (both under the resolver's lock): the word resolve()'s callers wait on.
        std::atomic<std::uint64_t> done{0};
        lookup_result              result{std::unexpected(std::error_code{})};

        // news is bumped (under the resolver's lock) when the first family's addresses come in, and when it's done:
        // the word resolutions wait on, notified only if one has joined (watched).
        std::atomic<std::uint64_t> news{0};

        // under the resolver's lock:
        // waiters is how many callers are still waiting (as callers join under it): the last to leave requests stop.
        std::size_t      waiters = 1;
        std::stop_source stop;
        bool             watched = false;

        // in a lookup of both families, the first family's to come in (first_family's, at first_at): the system back
        // end's result for it (an error too, which isn't told of), or the native's addresses.
        std::optional<lookup_result> first{};
        protocol                     first_family = protocol::not_care;
        io::clock::time_point        first_at{};

        // the words resolutions watch (see resolution::watch()), bumped when news next is.
        std::vector<watcher> watchers{};

        [[nodiscard]] lookup_key key() const noexcept
        {
            return {.scheduler = scheduler, .host = host, .proto = proto, .use = use};
        }
    };

    struct resolver_state
    {
        explicit resolver_state(resolver::options opts)
            : use{opts.use == backend::automatic ? fixed_choice(opts.facts).value_or(backend::automatic) : opts.use}
            , facts{std::move(opts.facts)}
            , system_lookup{std::move(opts.system_lookup)}
            , configs{std::move(opts.conf), std::move(opts.files)}
        {}

        // the back end: automatic only if it's chosen per name (see fixed_choice()).
        resolver::backend use = resolver::backend::automatic;
        system_facts      facts;
        lookup_function   system_lookup;

        // the native back end's configuration, and the server its next lookup starts with, if it rotates them.
        config_cache             configs;
        std::atomic<std::size_t> next_server{0};

        // in_flight are the lookups not yet finished, which callers for the same key join (as Go's singleflight), each
        // keyed by its own fields. A lookup leaves it when it finishes, or when its last caller leaves (Go's
        // ForgetUnshared), whichever is first. mu also guards what's under it in each lookup.
        std::mutex                                                               mu;
        std::unordered_map<lookup_key, std::shared_ptr<lookup>, lookup_key_hash> in_flight;
    };

}

namespace
{

    using detail::lookup;

    // forget takes state out of the resolver's table, if it's still there, so later callers start another lookup
    // rather than join it. Under the resolver's lock.
    void forget(detail::resolver_state& resolver, const lookup& state) noexcept
    {
        const auto it = resolver.in_flight.find(state.key());
        if (it != resolver.in_flight.end() && it->second.get() == &state) resolver.in_flight.erase(it);
    }

    // news is what a lookup tells its resolutions when it has something new for them: whether to notify its news word,
    // and the words watching it, to bump and notify.
    struct news
    {
        bool                 watched = false;
        std::vector<watcher> watchers{};
    };

    // tell bumps state's news word, under the resolver's lock, returning what to notify once the lock's released.
    news tell(lookup& state) noexcept
    {
        state.news.fetch_add(1, std::memory_order::release);
        return {.watched = state.watched, .watchers = std::exchange(state.watchers, {})};
    }

    // notify notifies what tell() bumped, holding no lock (as a notification resumes waiters inline once the pool's
    // stopped).
    void notify(const lookup& state, const news& told) noexcept
    {
        if (told.watched) state.scheduler->notify_all(state.news);
        for (const auto& word : told.watchers)
        {
            word->fetch_add(1, std::memory_order::release);
            state.scheduler->notify_all(*word);
        }
    }

    // finish hands a lookup's result to the callers that are left (if any), once it's forgotten, so a caller that
    // comes after starts another.
    void finish(detail::resolver_state& resolver, lookup& state, lookup_result result) noexcept
    {
        news told;
        {
            const std::scoped_lock lock{resolver.mu};
            forget(resolver, state);

            state.result = std::move(result);
            state.done.store(1, std::memory_order::release);
            told = tell(state);
        }

        state.scheduler->notify_all(state.done);
        notify(state, told);
    }

    // leave takes a caller that gives up off a lookup's count. The last to leave forgets the lookup in the same step
    // (Go's singleflight's ForgetUnshared), so no caller can join it once it's being stopped, and get the failure that
    // follows (operation_canceled) for another caller's reason; then it stops it, holding no lock (as the scheduler's
    // waits require).
    void leave(detail::resolver_state& resolver, lookup& state) noexcept
    {
        {
            const std::scoped_lock lock{resolver.mu};
            if (--state.waiters != 0) return;
            forget(resolver, state);
        }

        state.stop.request_stop();
    }

    // keep_first keeps the first family's result to come in, in a lookup of both (the system back end's), telling the
    // resolutions of its addresses (if it has any). If there's one already, it takes it back and returns it instead:
    // result is the other's, which finishes the lookup with it. (Until it does, a resolution that looks finds
    // neither, and waits for the result, which is told of: those that found the first already have it.)
    std::optional<lookup_result>
    keep_first(detail::resolver_state& resolver, lookup& state, protocol family, lookup_result& result) noexcept
    {
        news told;
        {
            const std::scoped_lock lock{resolver.mu};
            if (state.first.has_value()) return std::exchange(state.first, std::nullopt);

            state.first        = std::move(result);
            state.first_family = family;
            state.first_at     = io::clock::now();
            if (!state.first->has_value()) return std::nullopt;

            told = tell(state);
        }

        notify(state, told);
        return std::nullopt;
    }

    // found_addresses keeps a family's addresses, in a lookup of both (the native back end's), as soon as they're in
    // (the other's question still open), telling the resolutions, unless it has some already.
    void found_addresses(detail::resolver_state& resolver,
                         lookup&                 state,
                         protocol                family,
                         std::vector<ip_addr>    addrs) noexcept
    {
        news told;
        {
            const std::scoped_lock lock{resolver.mu};
            if (state.first.has_value() || addrs.empty()) return;

            state.first.emplace(std::move(addrs));
            state.first_family = family;
            state.first_at     = io::clock::now();
            told               = tell(state);
        }

        notify(state, told);
    }

    // both_failed is the error of a lookup of both families (the system back end's) when each family's failed: a
    // temporary failure's if only one is one (the name may well have addresses: as getaddrinfo() for both reports a
    // query that timed out, though the other said there's no such name), and otherwise IPv4's (with AI_ADDRCONFIG,
    // IPv6's is the likelier to be the host's own: it has no IPv6 address).
    std::error_code both_failed(const std::error_code& ipv6, const std::error_code& ipv4) noexcept
    {
        if (ipv6 == condition::temporary_failure && ipv4 != condition::temporary_failure) return ipv6;
        return ipv4;
    }

    // combined is a lookup of both families' result (the system back end's), from each's: their addresses, sorted by
    // RFC 6724 together (getaddrinfo() sorted each family's, but not the two together), or one's if the other failed,
    // or both_failed()'s error.
    lookup_result combined(lookup_result ipv6, lookup_result ipv4) noexcept
    {
        if (!ipv6.has_value() && !ipv4.has_value()) return std::unexpected(both_failed(ipv6.error(), ipv4.error()));
        if (!ipv6.has_value()) return ipv4;
        if (!ipv4.has_value()) return ipv6;

        try
        {
            auto addrs = std::move(*ipv6);
            addrs.insert(addrs.end(), ipv4->begin(), ipv4->end());
            detail::sort_by_rfc6724(addrs);
            return addrs;
        }
        catch (...) // failing to allocate
        {
            return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
        }
    }

    // family_done takes one family's result, in a lookup of both (the system back end's): the first is kept (and its
    // addresses told of), and the second makes the lookup's result with it.
    void family_done(detail::resolver_state& resolver, lookup& state, protocol family, lookup_result result) noexcept
    {
        auto earlier = keep_first(resolver, state, family, result);
        if (!earlier.has_value()) return;

        if (family == protocol::ipv6) finish(resolver, state, combined(std::move(result), std::move(*earlier)));
        else finish(resolver, state, combined(std::move(*earlier), std::move(result)));
    }

    // look_up_system looks state's host up with the system back end, on one of the scheduler's threads for blocking
    // calls: proto's addresses (in a lookup of both families, those of one of them). Not started at all if every caller
    // has left by the time it has a thread.
    coro::task<lookup_result>
    look_up_system(const detail::resolver_state* resolver, const lookup* state, protocol proto)
    {
        auto ran = co_await state->scheduler->run_blocking(
            [resolver, state, proto]
            {
                return resolver->system_lookup ? resolver->system_lookup(state->host, proto)
                                               : getaddrinfo_lookup(state->host, proto, state->proto);
            },
            state->stop.get_token());

        if (!ran.has_value()) co_return std::unexpected(ran.error());
        if (!ran->has_value() || proto == state->proto) co_return std::move(*ran);

        // one family of both: only its addresses (not a numeric host's of the other family, or a test's).
        auto& addrs = **ran;
        std::erase_if(addrs, [proto](const ip_addr& addr) { return !of_protocol(addr, proto); });
        if (addrs.empty()) co_return std::unexpected(make_gai_error_code(EAI_NONAME));
        co_return std::move(*ran);
    }

    // run_family looks one family up, in a lookup of both (the system back end's), and hands its result over.
    coro::task<>
    run_family(std::shared_ptr<detail::resolver_state> resolver, std::shared_ptr<lookup> state, protocol family)
    {
        lookup_result result{std::unexpected(std::make_error_code(std::errc::not_enough_memory))};
        try
        {
            result = co_await look_up_system(resolver.get(), state.get(), family);
        }
        catch (...) // NOLINT(bugprone-empty-catch): failing to allocate: result says so.
        {}

        family_done(*resolver, *state, family, std::move(result));
    }

    // run_lookup does a lookup's work, on its own (the callers wait for it, and may leave), and hands them its result.
    // A native lookup works from conf.
    coro::task<> run_lookup(std::shared_ptr<detail::resolver_state> resolver,
                            std::shared_ptr<lookup>                 state,
                            std::shared_ptr<const config>           conf)
    {
        if (state->use == backend::system && state->proto == protocol::not_care)
        {
            // both families, as two calls, each on a thread of its own: IPv4's in a task of its own, IPv6's here.
            // NOTE: the scheduler refuses the task only once it's stopping, which is a stop for every caller.
            auto refused = std::make_error_code(std::errc::operation_canceled);
            try
            {
                if (state->scheduler->spawn(run_family(resolver, state, protocol::ipv4))) refused.clear();
            }
            catch (...)
            {
                refused = std::make_error_code(std::errc::not_enough_memory);
            }
            if (refused) family_done(*resolver, *state, protocol::ipv4, std::unexpected(refused));

            co_await run_family(std::move(resolver), std::move(state), protocol::ipv6);
            co_return;
        }

        lookup_result result{std::unexpected(std::make_error_code(std::errc::not_enough_memory))};

        try
        {
            if (state->use == backend::native)
            {
                const auto first = conf->rotate ? resolver->next_server.fetch_add(1, std::memory_order::relaxed) : 0;

                // a family's addresses, as soon as they're in, if the other's question is still open: told of.
                const auto found =
                    [resolver = resolver.get(), state = state.get()](protocol family, std::vector<ip_addr> addrs)
                { found_addresses(*resolver, *state, family, std::move(addrs)); };

                result = co_await detail::native_lookup(state->scheduler,
                                                        std::move(conf),
                                                        state->host,
                                                        state->proto,
                                                        first,
                                                        state->stop.get_token(),
                                                        found);
            }
            else
            {
                result = co_await look_up_system(resolver.get(), state.get(), state->proto);
            }
        }
        catch (...) // NOLINT(bugprone-empty-catch): failing to allocate (or to read the configuration): result says so.
        {}

        finish(*resolver, *state, std::move(result));
    }

    // choice is the back end a lookup uses, and the configuration a native one works from.
    struct choice
    {
        backend                       use = backend::system;
        std::shared_ptr<const config> conf{};
    };

    // choose_for is the back end to look host (lower-cased) up with: the resolver's, or for automatic, the one Go's
    // rule chooses for it from the configuration as it is now (read again if it has changed).
    std::expected<choice, std::error_code> choose_for(detail::resolver_state& resolver, std::string_view host) noexcept
    {
        try
        {
            choice c{.use = resolver.use};
            if (c.use == backend::system) return c;

            // NOTE: the configuration as it is when the lookup starts, kept until it ends.
            c.conf = resolver.configs.get();
            if (c.use == backend::automatic) c.use = detail::choose(*c.conf, host, resolver.facts);
            if (c.use == backend::system) c.conf.reset();
            return c;
        }
        catch (...) // failing to allocate (or to read the configuration)
        {
            return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
        }
    }

    // to_lower lower-cases host in place: DNS names are case-insensitive, and so are hosts files' to the C libraries,
    // so a lookup holds one string, lower-cased, which its key views.
    void to_lower(std::string& host) noexcept
    {
        std::ranges::transform(host, host.begin(), [](char c) { return util::to_lower_ascii(c); });
    }

    // join joins the lookup of host (lower-cased) in flight, or starts one (spawning its task), for a caller: a
    // resolution, if watching. It throws only if it can't allocate, before joining.
    std::shared_ptr<lookup> join(io::scheduler*                                 scheduler,
                                 const std::shared_ptr<detail::resolver_state>& resolver,
                                 std::string                                    host,
                                 protocol                                       proto,
                                 choice                                         chosen,
                                 bool                                           watching)
    {
        std::shared_ptr<lookup> state;
        bool                    started = false;
        {
            const std::scoped_lock lock{resolver->mu};

            const auto it =
                resolver->in_flight.find({.scheduler = scheduler, .host = host, .proto = proto, .use = chosen.use});
            if (it != resolver->in_flight.end())
            {
                state = it->second;
                ++state->waiters;
            }
            else
            {
                state = std::make_shared<lookup>(scheduler, std::move(host), proto, chosen.use);
                resolver->in_flight.emplace(state->key(), state);
                started = true;
            }

            if (watching) state->watched = true;
        }

        if (started)
        {
            // NOTE: the scheduler refuses it once it's stopping, which is a stop for every caller on it.
            auto refused = std::make_error_code(std::errc::operation_canceled);
            try
            {
                if (scheduler->spawn(run_lookup(resolver, state, std::move(chosen.conf)))) refused.clear();
            }
            catch (...)
            {
                refused = std::make_error_code(std::errc::not_enough_memory);
            }

            if (refused) finish(*resolver, *state, std::unexpected(refused));
        }

        return state;
    }

    // look_up joins the lookup of host in flight, or starts one, and waits for its result, leaving if the deadline
    // passes or stop is requested first.
    coro::task<lookup_result> look_up(io::scheduler*                          scheduler,
                                      std::shared_ptr<detail::resolver_state> resolver,
                                      std::string                             host,
                                      resolve_options                         options)
    {
        to_lower(host);

        auto chosen = choose_for(*resolver, host);
        if (!chosen.has_value()) co_return std::unexpected(chosen.error());

        auto state = join(scheduler, resolver, std::move(host), options.proto, std::move(*chosen), false);

        // leaves if this ends without the result: the deadline, a stop, or this coroutine destroyed while it waits.
        util::defer leave_early{[&resolver, &state] noexcept
                                {
                                    if (state->done.load(std::memory_order::acquire) == 0) leave(*resolver, *state);
                                }};

        while (state->done.load(std::memory_order::acquire) == 0)
        {
            auto err = co_await scheduler->wait_on(state->done, 0, options.deadline, options.stop);
            if (err) co_return std::unexpected(err);
        }

        co_return state->result;
    }

}

namespace detail
{

    resolution::resolution(std::vector<ip_addr> addrs) noexcept
        : addrs{std::move(addrs)}
    {}

    resolution::resolution(std::shared_ptr<resolver_state> resolver, std::shared_ptr<lookup> state) noexcept
        : resolver{std::move(resolver)}
        , state{std::move(state)}
    {}

    resolution::resolution(resolution&& other) noexcept
        : resolver{std::move(other.resolver)}
        , state{std::move(other.state)}
        , addrs{std::move(other.addrs)}
        , watching{std::exchange(other.watching, nullptr)}
    {}

    resolution::~resolution()
    {
        if (!state) return;

        // NOTE: the word's owner may go with it, so it's let go of once the lock's released.
        watcher unwatched;
        if (watching != nullptr)
        {
            const std::scoped_lock lock{resolver->mu};

            const auto it =
                std::ranges::find_if(state->watchers, [this](const watcher& w) { return w.get() == watching; });
            if (it != state->watchers.end())
            {
                unwatched = std::move(*it);
                state->watchers.erase(it);
            }
        }

        if (state->done.load(std::memory_order::acquire) == 0) leave(*resolver, *state);
    }

    found resolution::look() const
    {
        if (!state) return {.done = true, .addrs = addrs};

        const std::scoped_lock lock{resolver->mu};

        found now{.version = state->news.load(std::memory_order::relaxed)};
        if (state->done.load(std::memory_order::relaxed) != 0)
        {
            now.done = true;
            if (state->result.has_value()) now.addrs = *state->result;
            else now.err = state->result.error();
        }
        else if (state->first.has_value() && state->first->has_value() && !(*state->first)->empty())
        {
            now.addrs  = **state->first;
            now.family = state->first_family;
            now.at     = state->first_at;
        }

        return now;
    }

    const std::atomic<std::uint64_t>& resolution::news() const noexcept
    {
        // done at once: nothing more will come.
        constinit static const std::atomic<std::uint64_t> never{0};
        return state ? state->news : never;
    }

    bool resolution::watch(std::shared_ptr<std::atomic<std::uint64_t>> word, std::uint64_t version)
    {
        if (!state) return true; // nothing more will come

        watcher                replaced;
        const std::scoped_lock lock{resolver->mu};
        if (state->news.load(std::memory_order::relaxed) != version) return false;

        // NOTE: the word's owner holds it too, so it doesn't go with the one replaced (if any) under the lock.
        if (watching != nullptr)
        {
            const auto it =
                std::ranges::find_if(state->watchers, [this](const watcher& w) { return w.get() == watching; });
            if (it != state->watchers.end())
            {
                replaced = std::move(*it);
                state->watchers.erase(it);
            }
        }

        watching = word.get();
        state->watchers.push_back(std::move(word));
        return true;
    }

}

resolver::resolver()
    : resolver{options{}}
{}

resolver::resolver(options opts)
    : state{std::make_shared<detail::resolver_state>(std::move(opts))}
{}

coro::task<std::expected<std::vector<ip_addr>, std::error_code>>
resolver::resolve(io::scheduler* scheduler, std::string host, resolve_options options) const
{
    if (options.stop.stop_requested())
        return coro::ready_task<lookup_result>(std::unexpected(std::make_error_code(std::errc::operation_canceled)));

    if (auto result = at_once(host, options.proto); result.has_value())
        return coro::ready_task<lookup_result>(std::move(*result));

    return look_up(scheduler, state, std::move(host), std::move(options));
}

std::expected<detail::resolution, std::error_code>
resolver::start_resolving(io::scheduler* scheduler, std::string host, protocol proto) const
{
    if (auto result = at_once(host, proto); result.has_value())
    {
        if (!result->has_value()) return std::unexpected(result->error());
        return detail::resolution{std::move(**result)};
    }

    to_lower(host);

    auto chosen = choose_for(*state, host);
    if (!chosen.has_value()) return std::unexpected(chosen.error());

    try
    {
        return detail::resolution{state, join(scheduler, state, std::move(host), proto, std::move(*chosen), true)};
    }
    catch (...) // failing to allocate
    {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

resolver& default_resolver()
{
    static resolver the_resolver;
    return the_resolver;
}

}
