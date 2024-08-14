#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/event_loop.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"

namespace net::io
{

class scheduler
{
public:
    class operation
    {
        friend class scheduler;

        explicit operation(scheduler* scheduler) noexcept
            : scheduler{scheduler}
        {}

    public:
        constexpr bool await_ready() noexcept { return false; }
        void           await_suspend(std::coroutine_handle<> awaiting) noexcept { scheduler->workers.resume(awaiting); }
        void           await_resume() noexcept { /* noop */ }

    private:
        scheduler* scheduler;
    };

    scheduler(std::size_t concurrency = std::thread::hardware_concurrency());

    scheduler(const scheduler&)            = delete;
    scheduler& operator=(const scheduler&) = delete;

    scheduler(scheduler&&)            = delete;
    scheduler& operator=(scheduler&&) = delete;

    ~scheduler() noexcept;

    operation          schedule() noexcept { return operation{this}; }
    void               schedule(coro::task<>&& task) noexcept;
    coro::task<result> schedule(io_handle handle, poll_op op, std::chrono::milliseconds timeout);
    bool               resume(std::coroutine_handle<> handle) noexcept;
    void               run();

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
    coro::thread_pool        workers;
    std::deque<coro::task<>> tasks;
    event_loop               loop;
    std::atomic<bool>        running;
    std::mutex               tasks_mu;
};

}
