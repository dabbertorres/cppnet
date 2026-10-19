#include "dns/resolver.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
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

namespace
{

using net::dns::condition;
using net::dns::errc;

using lookup_result = std::expected<std::vector<net::ip_addr>, std::error_code>;

class dns_category final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "net::dns"; }

    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<errc>(ev))
        {
        case errc::name_not_found: return "name not found";
        case errc::no_address: return "no address of the family asked for";
        case errc::server_failure: return "the name servers failed";
        case errc::refused: return "the name servers refused";
        case errc::bad_response: return "bad response from a name server";
        default: return "(unrecognized)";
        }
    }

    [[nodiscard]] std::error_condition default_error_condition(int ev) const noexcept override
    {
        switch (static_cast<errc>(ev))
        {
        case errc::name_not_found:
        case errc::no_address: return make_error_condition(condition::not_found);
        case errc::server_failure: return make_error_condition(condition::temporary_failure);
        default: return {ev, *this};
        }
    }
};

class condition_category final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "net::dns::condition"; }

    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<condition>(ev))
        {
        case condition::not_found: return "not found";
        case condition::temporary_failure: return "temporary failure";
        default: return "(unrecognized)";
        }
    }

    // NOTE: errc's codes match by their default_error_condition(), which the comparison also checks; this matches
    //       getaddrinfo()'s.
    [[nodiscard]] bool equivalent(const std::error_code& code, int cond) const noexcept override
    {
        if (code.category() != net::gai_category()) return false;

        const auto err = code.value();
        switch (static_cast<condition>(cond))
        {
        case condition::not_found:
#ifdef EAI_NODATA
            // NOTE: not a switch: EAI_NODATA may be EAI_NONAME's value, where it's kept for compatibility.
            if (err == EAI_NODATA) return true;
#endif
            return err == EAI_NONAME;

        case condition::temporary_failure: return err == EAI_AGAIN || err == EAI_FAIL;

        default: return false;
        }
    }
};

const dns_category       the_dns_category;
const condition_category the_condition_category;

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

// lookup is a lookup in flight: its result, once it has one, and its callers.
struct lookup
{
    // done is 1 once result is set: the word callers wait on.
    std::atomic<std::uint64_t> done{0};
    lookup_result              result{std::unexpected(std::error_code{})};

    // waiters is how many callers are still waiting: the last to leave requests stop.
    std::atomic<std::size_t> waiters{1};
    std::stop_source         stop;

    // leave takes a caller that gives up off the count, stopping the lookup if it was the last. Stop is requested
    // holding no lock (as the scheduler's waits require).
    void leave() noexcept
    {
        if (waiters.fetch_sub(1, std::memory_order::acq_rel) == 1) stop.request_stop();
    }
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
    };

}

namespace
{

    // run_lookup does a lookup's work, on its own (the callers wait for it, and may leave), and stores its result.
    coro::task<> run_lookup(io::scheduler*                                scheduler,
                            std::shared_ptr<const detail::resolver_state> resolver,
                            std::shared_ptr<lookup>                       state,
                            std::string                                   host,
                            protocol                                      proto)
    {
        lookup_result result{std::unexpected(std::make_error_code(std::errc::not_enough_memory))};

        try
        {
            // NOTE: the system's back end, whatever resolver->use is, until there's another.
            //       Not started at all if every caller has left by the time it has a thread.
            auto ran = co_await scheduler->run_blocking(
                [&]
                {
                    return resolver->system_lookup ? resolver->system_lookup(host, proto)
                                                   : getaddrinfo_lookup(host, proto);
                },
                state->stop.get_token());

            if (ran.has_value()) result = std::move(*ran);
            else result = std::unexpected(ran.error());
        }
        catch (...) // NOLINT(bugprone-empty-catch): failing to allocate, the only way it can throw: result says so.
        {}

        // the callers that are left (if any) take the result once they see done.
        state->result = std::move(result);
        state->done.store(1, std::memory_order::release);
        scheduler->notify_all(state->done);
    }

    // look_up starts a lookup of host, and waits for its result, leaving if the deadline passes or stop is requested
    // first.
    coro::task<lookup_result> look_up(io::scheduler*                                scheduler,
                                      std::shared_ptr<const detail::resolver_state> resolver,
                                      std::string                                   host,
                                      resolve_options                               options)
    {
        auto state = std::make_shared<lookup>();

        // NOTE: the scheduler refuses it once it's stopping, which is a stop too.
        if (!scheduler->spawn(run_lookup(scheduler, std::move(resolver), state, std::move(host), options.proto)))
            co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));

        // leaves if this ends without the result: the deadline, a stop, or this coroutine destroyed while it waits.
        util::defer leave{[&state] noexcept
                          {
                              if (state->done.load(std::memory_order::acquire) == 0) state->leave();
                          }};

        while (state->done.load(std::memory_order::acquire) == 0)
        {
            auto err = co_await scheduler->wait_on(state->done, 0, options.deadline, options.stop);
            if (err) co_return std::unexpected(err);
        }

        co_return state->result;
    }

}

const std::error_category& category() noexcept { return the_dns_category; }

std::error_code make_error_code(errc err) noexcept { return {static_cast<int>(err), the_dns_category}; }

const std::error_category& condition_category() noexcept { return the_condition_category; }

std::error_condition make_error_condition(condition cond) noexcept
{
    return {static_cast<int>(cond), the_condition_category};
}

resolver::resolver()
    : resolver{options{}}
{}

resolver::resolver(options opts)
    : state{std::make_shared<const detail::resolver_state>(opts.use, std::move(opts.system_lookup))}
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
