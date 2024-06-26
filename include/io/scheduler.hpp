#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
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

    auto schedule(wait_for job) noexcept
    {
        struct awaitable
        {
            event_loop* loop = nullptr;
            wait_for    job;

            constexpr bool await_ready() noexcept { return false; }
            constexpr void await_resume() noexcept {}
            void           await_suspend(std::coroutine_handle<> handle) noexcept
            {
                job.handle = handle;
                loop->queue(job);
            }
        };

        return awaitable{&loop, job};
    }

    /* auto schedule_accept(int fd) noexcept */
    /* { */
    /*     struct awaitable */
    /*     { */
    /*         event_loop* loop = nullptr; */
    /*         int         fd   = -1; */

    /*         constexpr bool await_ready() noexcept { return false; } */
    /*         constexpr void await_resume() noexcept {} */
    /*         void           await_suspend(std::coroutine_handle<> handle) const noexcept { loop->queue(handle, fd); }
     */
    /*     }; */

    /*     return awaitable{&loop, fd}; */
    /* } */

    void shutdown() noexcept;

    /* std::size_t size() noexcept; */

private:
    void io_loop();

    coro::thread_pool workers;
    std::thread       io;

    event_loop        loop;
    std::atomic<bool> shutdown_please;
};

}
