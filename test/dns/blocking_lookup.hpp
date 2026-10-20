#pragma once

#include <atomic>
#include <chrono>
#include <expected>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "dns/resolver.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"

// A system lookup that blocks until it's released, for the tests of what gives up on one (the resolver's, connect's,
// and the HTTP client's).
namespace test
{

// blocking_lookup is a resolver whose lookups block, holding their thread, until release() is called, then answer.
// It must outlive the scheduler the lookups run on (whose shutdown() waits for them), and be released before then: e.g.
// by a util::defer declared after the scheduler, so a failing REQUIRE doesn't leave the scheduler waiting.
struct blocking_lookup
{
    std::atomic<bool> released{false};
    std::atomic<int>  started{0};
    std::atomic<int>  finished{0};

    // what the lookups return, once released.
    std::expected<std::vector<net::ip_addr>, std::error_code> answer{
        std::vector<net::ip_addr>{net::ipv4_addr::loopback()}};

    net::dns::resolver resolver{
        {
         .use = net::dns::resolver::backend::system,
         .system_lookup =
                [this](const std::string& /*host*/, net::protocol /*proto*/)
            {
                started.fetch_add(1);
                started.notify_all();

                released.wait(false);

                finished.fetch_add(1);
                finished.notify_all();
                return answer;
            }, }
    };

    blocking_lookup() = default;

    // non-copyable and non-movable: the lookups refer to it.
    blocking_lookup(const blocking_lookup&)            = delete;
    blocking_lookup& operator=(const blocking_lookup&) = delete;
    blocking_lookup(blocking_lookup&&)                 = delete;
    blocking_lookup& operator=(blocking_lookup&&)      = delete;

    ~blocking_lookup() = default;

    void release() noexcept
    {
        released.store(true);
        released.notify_all();
    }

    // wait_started blocks until n lookups have started (been given a thread), returning false if they haven't within
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
