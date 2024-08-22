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
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"

namespace net::io
{

scheduler::scheduler(std::shared_ptr<coro::thread_pool> workers, const std::shared_ptr<spdlog::logger>& logger)
    : workers{std::move(workers)}
    , loop{logger}
    , running{true}
    , logger{logger->clone("scheduler")}
    , run_worker{&scheduler::run, this}
{}

scheduler::~scheduler() noexcept { shutdown(); }

void scheduler::register_handle(handle handle) { loop.register_handle(handle); }
void scheduler::deregister_handle(handle handle) { loop.deregister_handle(handle); }

bool scheduler::schedule(coro::task<>&& task) noexcept
{
    std::coroutine_handle<> handle;

    {
        std::lock_guard lock{tasks_mu};
        handle = tasks.emplace_back(std::move(task)).get_handle();
    }

    logger->trace("[c {}]: scheduling; done = {}", handle.address(), handle.done());
    return workers->resume(handle);
}

bool scheduler::resume(std::coroutine_handle<> handle) noexcept
{
    if (handle == nullptr) return false;
    if (!running.load(std::memory_order::acquire)) return false;

    logger->trace("[c {}]: resuming; done = {}", handle.address(), handle.done());
    return workers->resume(handle);
}

coro::task<result> scheduler::schedule(handle handle, poll_op op, std::chrono::milliseconds timeout)
{
    return loop.queue(handle, op, timeout);
}

// TODO: timeout
void scheduler::shutdown() noexcept
{
    if (running.exchange(false, std::memory_order::acq_rel))
    {
        logger->trace("shutting down - waiting on loop to shutdown...");
        loop.shutdown();

        std::lock_guard lock{tasks_mu};
        for (auto& t : tasks)
        {
            if (t.valid())
            {
                if (!t.resume()) t.destroy();
            }
        }

        logger->trace("shutting down - waiting on run() to exit...");
        if (run_worker.joinable()) run_worker.join();
    }
}

void scheduler::run()
{
    while (running.load(std::memory_order::acquire))
    {
        for (auto [handle, result] : loop.dispatch())
        {
            handle.promise().return_value(result); // to move or not to move?
            logger->trace("[c {}]: resuming", handle.address());
            workers->resume(handle);
        }

        // TODO: any better way to do this?
        std::lock_guard lock{tasks_mu};
        std::erase_if(tasks, [](const coro::task<>& t) { return t.is_ready(); });
    }
}

}
