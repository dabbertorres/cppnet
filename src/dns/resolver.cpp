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

// getaddrinfo_lookup looks host up with getaddrinfo(), blocking until it's done.
lookup_result getaddrinfo_lookup(const std::string& host, net::protocol proto) noexcept
{
    // NOTE: not a designated initializer, as addrinfo's fields are in a different order on each platform.
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM; // one of each address, rather than one for each kind of socket.

    switch (proto)
    {
    case net::protocol::not_care: hints.ai_family = AF_UNSPEC; break;
    case net::protocol::ipv4: hints.ai_family = AF_INET; break;
    case net::protocol::ipv6: hints.ai_family = AF_INET6; break;
    }

    // An address (in a form literal() leaves to getaddrinfo(), e.g. "127.1") is read as one. A name only resolves to
    // the kinds of address this host has (AI_ADDRCONFIG), but that would also reject an IPv6 address when there's no
    // IPv6 network configured (glibc doesn't count loopback addresses).
    addrinfo* found = nullptr;

    hints.ai_flags = AI_NUMERICHOST;
    auto err       = ::getaddrinfo(host.c_str(), nullptr, &hints, &found);
    if (err == EAI_NONAME)
    {
        hints.ai_flags = AI_ADDRCONFIG;
        err            = ::getaddrinfo(host.c_str(), nullptr, &hints, &found);
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

// lookup_key is what makes two lookups the same, so concurrent callers share one: the scheduler (whose word the
// callers wait on), the name (lower-cased: DNS names are case-insensitive) and the families. (The back end is the
// resolver's, whose table it is.)
struct lookup_key
{
    net::io::scheduler* scheduler = nullptr;
    std::string_view    host{};
    net::protocol       proto = net::protocol::not_care;

    bool operator==(const lookup_key&) const = default;
};

struct lookup_key_hash
{
    std::size_t operator()(const lookup_key& key) const noexcept
    {
        auto hash = net::util::detail::hash_combine(0, key.host);
        hash      = net::util::detail::hash_combine(hash, key.scheduler);
        return net::util::detail::hash_combine(hash, key.proto);
    }
};

// lookup is a lookup in flight: what it looks up, its result once it has one, and its callers.
struct lookup
{
    lookup(net::io::scheduler* scheduler, std::string host, net::protocol proto) noexcept
        : scheduler{scheduler}
        , host{std::move(host)}
        , proto{proto}
    {}

    net::io::scheduler* const scheduler;
    const std::string         host; // lower-cased
    const net::protocol       proto;

    // done is 1 once result is set: the word callers wait on.
    std::atomic<std::uint64_t> done{0};
    lookup_result              result{std::unexpected(std::error_code{})};

    // waiters is how many callers are still waiting (under the resolver's lock, as callers join under it): the last to
    // leave requests stop.
    std::size_t      waiters = 1;
    std::stop_source stop;

    [[nodiscard]] lookup_key key() const noexcept { return {.scheduler = scheduler, .host = host, .proto = proto}; }
};

}

namespace net::dns
{

namespace detail
{

    struct resolver_state
    {
        resolver::backend use = resolver::backend::automatic;
        lookup_function   system_lookup;

        // in_flight are the lookups not yet finished, which callers for the same key join (as Go's singleflight), each
        // keyed by its own fields. A lookup leaves it when it finishes, or when its last caller leaves (Go's
        // ForgetUnshared), whichever is first.
        std::mutex                                                               mu;
        std::unordered_map<lookup_key, std::shared_ptr<lookup>, lookup_key_hash> in_flight;
    };

}

namespace
{

    // forget takes state out of the resolver's table, if it's still there, so later callers start another lookup
    // rather than join it. Under the resolver's lock.
    void forget(detail::resolver_state& resolver, const lookup& state) noexcept
    {
        const auto it = resolver.in_flight.find(state.key());
        if (it != resolver.in_flight.end() && it->second.get() == &state) resolver.in_flight.erase(it);
    }

    // finish hands a lookup's result to the callers that are left (if any), once it's forgotten, so a caller that
    // comes after starts another.
    void finish(detail::resolver_state& resolver, lookup& state, lookup_result result) noexcept
    {
        {
            const std::scoped_lock lock{resolver.mu};
            forget(resolver, state);
        }

        state.result = std::move(result);
        state.done.store(1, std::memory_order::release);
        state.scheduler->notify_all(state.done);
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

    // run_lookup does a lookup's work, on its own (the callers wait for it, and may leave), and hands them its result.
    coro::task<> run_lookup(std::shared_ptr<detail::resolver_state> resolver, std::shared_ptr<lookup> state)
    {
        lookup_result result{std::unexpected(std::make_error_code(std::errc::not_enough_memory))};

        try
        {
            // NOTE: the system's back end, whatever resolver->use is, until there's another.
            //       Not started at all if every caller has left by the time it has a thread.
            auto ran = co_await state->scheduler->run_blocking(
                [&]
                {
                    return resolver->system_lookup ? resolver->system_lookup(state->host, state->proto)
                                                   : getaddrinfo_lookup(state->host, state->proto);
                },
                state->stop.get_token());

            if (ran.has_value()) result = std::move(*ran);
            else result = std::unexpected(ran.error());
        }
        catch (...) // NOLINT(bugprone-empty-catch): failing to allocate, the only way it can throw: result says so.
        {}

        finish(*resolver, *state, std::move(result));
    }

    // look_up joins the lookup of host in flight, or starts one, and waits for its result, leaving if the deadline
    // passes or stop is requested first.
    coro::task<lookup_result> look_up(io::scheduler*                          scheduler,
                                      std::shared_ptr<detail::resolver_state> resolver,
                                      std::string                             host,
                                      resolve_options                         options)
    {
        // NOTE: looked up lower-cased too (DNS names are case-insensitive, and so are hosts files' to the C libraries),
        //       so the lookup holds the one string, its key viewing it.
        std::ranges::transform(host, host.begin(), [](char c) { return util::to_lower_ascii(c); });

        std::shared_ptr<lookup> state;
        bool                    started = false;
        {
            const std::scoped_lock lock{resolver->mu};

            const auto it = resolver->in_flight.find({.scheduler = scheduler, .host = host, .proto = options.proto});
            if (it != resolver->in_flight.end())
            {
                state = it->second;
                ++state->waiters;
            }
            else
            {
                state = std::make_shared<lookup>(scheduler, std::move(host), options.proto);
                resolver->in_flight.emplace(state->key(), state);
                started = true;
            }
        }

        // leaves if this ends without the result: the deadline, a stop, or this coroutine destroyed while it waits.
        util::defer leave_early{[&resolver, &state] noexcept
                                {
                                    if (state->done.load(std::memory_order::acquire) == 0) leave(*resolver, *state);
                                }};

        if (started)
        {
            // NOTE: the scheduler refuses it once it's stopping, which is a stop for every caller on it.
            auto refused = std::make_error_code(std::errc::operation_canceled);
            try
            {
                if (scheduler->spawn(run_lookup(resolver, state))) refused.clear();
            }
            catch (...)
            {
                refused = std::make_error_code(std::errc::not_enough_memory);
            }

            if (refused) finish(*resolver, *state, std::unexpected(refused));
        }

        while (state->done.load(std::memory_order::acquire) == 0)
        {
            auto err = co_await scheduler->wait_on(state->done, 0, options.deadline, options.stop);
            if (err) co_return std::unexpected(err);
        }

        co_return state->result;
    }

}

resolver::resolver()
    : resolver{options{}}
{}

resolver::resolver(options opts)
    : state{std::make_shared<detail::resolver_state>(opts.use, std::move(opts.system_lookup))}
{}

coro::task<std::expected<std::vector<ip_addr>, std::error_code>>
resolver::resolve(io::scheduler* scheduler, std::string host, resolve_options options) const
{
    if (options.stop.stop_requested())
        return coro::ready_task<lookup_result>(std::unexpected(std::make_error_code(std::errc::operation_canceled)));

    if (auto addr = literal(host); addr.has_value())
    {
        if (!of_protocol(*addr, options.proto))
        {
            return coro::ready_task<lookup_result>(
                std::unexpected(std::make_error_code(std::errc::address_family_not_supported)));
        }
        return coro::ready_task<lookup_result>(std::vector{*addr});
    }

    if (!valid_name(host))
        return coro::ready_task<lookup_result>(std::unexpected(std::make_error_code(std::errc::invalid_argument)));

    return look_up(scheduler, state, std::move(host), std::move(options));
}

resolver& default_resolver()
{
    static resolver the_resolver;
    return the_resolver;
}

}
