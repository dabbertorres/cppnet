#include "io/aio/scheduler.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <mutex>

#include <sys/epoll.h>

namespace net::io::aio
{

scheduler::scheduler(std::size_t concurrency)
    : workers{concurrency}
    , io{&scheduler::io_loop, this}
    , shutdown_please{false}
{}

scheduler::~scheduler() noexcept { shutdown(); }

void scheduler::process_events(std::chrono::milliseconds timeout)
{
    loop.process_events([this]() { process_scheduled_tasks(); },
                        [this]() { process_timeouts(); },
                        [this]() { process_event(); },
                        timeout);
}

void scheduler::shutdown() noexcept
{
    if (!shutdown_please.exchange(true, std::memory_order::acq_rel))
    {
        workers.shutdown();

        loop.shutdown();

        if (io.joinable()) io.join();
    }
}

void scheduler::process_scheduled_tasks()
{
    std::vector<std::coroutine_handle<>> tasks;
    {
        std::lock_guard lock{scheduled_tasks_mutex};
        tasks.swap(scheduled_tasks);

        loop.clear_schedule();

        schedule_triggered.exchange(false, std::memory_order::release);
    }

    std::for_each(tasks.begin(), tasks.end(), [](auto& task) { task.resume(); });
}

void scheduler::process_timeouts()
{
    // TODO
}

void scheduler::process_event(/*TODO*/)
{
    // TODO
}

void scheduler::io_loop()
{
    while (!shutdown_please.load(std::memory_order::acquire) || size() > 0)
    {
        process_events(1'000ms);
    }
}

}
