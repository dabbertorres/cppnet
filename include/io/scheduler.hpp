#pragma once

#include <atomic>
#include <thread>

#include "coro/thread_pool.hpp"
#include "io/event_loop.hpp"
#include "io/poll.hpp"

namespace net::io
{

using namespace std::chrono_literals;

class scheduler
{
public:
    scheduler(std::size_t concurrency = coro::hardware_concurrency(1));

    scheduler(const scheduler&)            = delete;
    scheduler& operator=(const scheduler&) = delete;

    scheduler(scheduler&&)            = delete;
    scheduler& operator=(scheduler&&) = delete;

    ~scheduler() noexcept;

    void schedule(wait_for job) noexcept;

    void shutdown() noexcept;

    std::size_t size() noexcept;

private:
    void io_loop();

    coro::thread_pool workers;
    std::thread       io;

    event_loop        loop;
    std::atomic<bool> shutdown_please;
};

}
