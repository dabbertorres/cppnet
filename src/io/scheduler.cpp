#include "io/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <spdlog/logger.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"

namespace net::io
{

scheduler::scheduler(std::shared_ptr<coro::thread_pool> workers, const std::shared_ptr<spdlog::logger>& logger)
    : workers{std::move(workers)}
    , loop{logger}
    , is_shutdown{false}
    , wait_for_shutdown{0}
{}

scheduler::~scheduler() noexcept { shutdown(); }

void scheduler::register_handle(handle handle) { loop.register_handle(handle); }
void scheduler::deregister_handle(handle handle) { loop.deregister_handle(handle); }

bool scheduler::schedule(coro::task<>&& task) noexcept
{
    auto handle = task.get_handle();

    {
        std::lock_guard lock{tasks_mu};
        tasks.emplace_back(std::move(task));
    }

    return workers->resume(handle);
}

bool scheduler::resume(std::coroutine_handle<> handle) noexcept
{
    if (handle == nullptr) return false;
    if (is_shutdown.test(std::memory_order::acquire)) return false;

    return workers->resume(handle);
}

coro::task<result> scheduler::schedule(handle handle, poll_op op, std::chrono::milliseconds timeout)
{
    return loop.queue(handle, op, timeout);
}

void scheduler::run()
{
    while (!is_shutdown.test(std::memory_order::acquire))
    {
        for (auto [handle, result] : loop.dispatch())
        {
            handle.promise().return_value(result);
            workers->resume(handle);
        }

        // TODO: any better way to do this?
        std::lock_guard lock{tasks_mu};
        std::erase_if(tasks, [](const coro::task<>& t) { return t.is_ready(); });
    }

    wait_for_shutdown.release();
}

void scheduler::run_until_done(coro::task<>&& task)
{
    if (is_shutdown.test(std::memory_order::acquire)) return;

    auto top_handle = task.get_handle();
    workers->resume(top_handle);

    while (top_handle && !top_handle.done())
    {
        for (auto [handle, result] : loop.dispatch())
        {
            handle.promise().return_value(result);
            workers->resume(handle);
        }
    }

    wait_for_shutdown.release();
}

void scheduler::shutdown(std::chrono::milliseconds timeout)
{
    if (is_shutdown.test_and_set(std::memory_order::release)) return;

    auto started_waiting = std::chrono::steady_clock::now();
    loop.shutdown(timeout);

    {
        std::lock_guard lock{tasks_mu};
        for (auto& t : tasks)
        {
            if (t.valid())
            {
                if (!t.is_ready())
                {
                    if (t.resume()) t.destroy();
                }
                else
                {
                    t.destroy();
                }
            }
        }
    }

    // now wait for shutdown to finish...
    if (timeout.count() != 0)
    {
        auto waited_for = std::chrono::steady_clock::now() - started_waiting;
        timeout -= std::chrono::duration_cast<decltype(timeout)>(waited_for);

        if (timeout.count() < 0)
        {
            // TODO: throw
        }

        auto did_acquire = wait_for_shutdown.try_acquire_for(timeout);
        if (!did_acquire)
        {
            // TODO: throw
        }
    }
    else
    {
        wait_for_shutdown.acquire();
    }
}

}
