#include "io/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/event_loop.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"

namespace net::io
{

scheduler::scheduler(std::size_t concurrency)
    : workers{concurrency}
    , running{true}
{}

scheduler::~scheduler() noexcept { shutdown(); }

void scheduler::schedule(coro::task<>&& task) noexcept
{
    std::lock_guard lock{tasks_mu};
    auto            handle = tasks.emplace_back(std::move(task)).get_handle();
    workers.resume(handle);
}

bool scheduler::resume(std::coroutine_handle<> handle) noexcept
{
    if (handle == nullptr) return false;
    if (!running.load(std::memory_order::acquire)) return false;

    return workers.resume(handle);
}

coro::task<result> scheduler::schedule(io_handle handle, poll_op op, std::chrono::milliseconds timeout)
{
    return loop.queue(handle, op, timeout);
}

void scheduler::run()
{
    while (running.load(std::memory_order::acquire) /* || size() > 0*/)
    {
        for (auto [handle, result] : loop.dispatch())
        {
            handle.promise().return_value(result); // to move or not to move?
            workers.resume(handle);
        }

        std::lock_guard lock{tasks_mu};
        std::erase_if(tasks, [](const coro::task<>& t) { return t.is_ready(); });
    }
}

void scheduler::shutdown() noexcept
{
    if (running.exchange(false, std::memory_order::acq_rel))
    {
        workers.shutdown();

        loop.shutdown();
    }
}

}
