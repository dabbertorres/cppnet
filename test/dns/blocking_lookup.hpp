#pragma once

#include <atomic>
#include <chrono>
#include <expected>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <netdb.h>

#include "dns/resolver.hpp"
#include "exception.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"

// A system lookup that blocks until it's released, for the tests of what gives up on one (the resolver's, connect's,
// and the HTTP client's).
namespace test
{

// blocking_lookup is a resolver whose lookups block, holding their thread, until release() is called, then answer.
// It must outlive the scheduler the lookups run on (whose shutdown() waits for them), and be released before then: e.g.
// by a util::defer declared after the scheduler, so a failing REQUIRE doesn't leave the scheduler waiting.
//
// A lookup of both families is two calls, one for each (see dns::detail::lookup_function), each blocking until its
// family is released (or both are): started and finished count the calls.
struct blocking_lookup
{
    std::atomic<bool> released_ipv4{false};
    std::atomic<bool> released_ipv6{false};
    std::atomic<int>  started{0};
    std::atomic<int>  finished{0};

    // what the lookups return, once released: a call for one family gets that family's addresses (or EAI_NONAME, if
    // there are none), or the error.
    std::expected<std::vector<net::ip_addr>, std::error_code> answer{
        std::vector<net::ip_addr>{net::ipv4_addr::loopback()}};

    net::dns::resolver resolver{
        {
         .use = net::dns::resolver::backend::system,
         .system_lookup =
                [this](const std::string& /*host*/, net::protocol proto)
            {
                started.fetch_add(1);
                started.notify_all();

                if (proto != net::protocol::ipv6) released_ipv4.wait(false);
                if (proto != net::protocol::ipv4) released_ipv6.wait(false);

                auto result = answer_for(proto);

                finished.fetch_add(1);
                finished.notify_all();
                return result;
            }, }
    };

    blocking_lookup() = default;

    // non-copyable and non-movable: the lookups refer to it.
    blocking_lookup(const blocking_lookup&)            = delete;
    blocking_lookup& operator=(const blocking_lookup&) = delete;
    blocking_lookup(blocking_lookup&&)                 = delete;
    blocking_lookup& operator=(blocking_lookup&&)      = delete;

    ~blocking_lookup() = default;

    // release releases proto's calls (both families', by default).
    void release(net::protocol proto = net::protocol::not_care) noexcept
    {
        if (proto != net::protocol::ipv6)
        {
            released_ipv4.store(true);
            released_ipv4.notify_all();
        }
        if (proto != net::protocol::ipv4)
        {
            released_ipv6.store(true);
            released_ipv6.notify_all();
        }
    }

    // answer_for is what a call for proto's addresses returns.
    [[nodiscard]] std::expected<std::vector<net::ip_addr>, std::error_code> answer_for(net::protocol proto) const
    {
        if (!answer.has_value() || proto == net::protocol::not_care) return answer;

        std::vector<net::ip_addr> addrs;
        for (const auto& addr : *answer)
        {
            if (addr.is_ipv4() == (proto == net::protocol::ipv4)) addrs.push_back(addr);
        }
        if (addrs.empty()) return std::unexpected(net::make_gai_error_code(EAI_NONAME));
        return addrs;
    }

    // wait_started blocks until n calls have started (been given a thread), returning false if they haven't within
    // 10 seconds. (Not from one of the scheduler's tasks, which would hold up its worker.)
    [[nodiscard]] bool wait_started(int n) const
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (started.load() < n)
        {
            if (std::chrono::steady_clock::now() >= until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return true;
    }
};

}
