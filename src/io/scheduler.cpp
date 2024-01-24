#include "io/scheduler.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>

#include "io/poll.hpp"

namespace net::io
{

scheduler::scheduler(std::size_t concurrency)
    : workers{concurrency}
    , io{&scheduler::io_loop, this}
    , shutdown_please{false}
{}

scheduler::~scheduler() noexcept { shutdown(); }

void scheduler::schedule(wait_for job) noexcept { loop.queue(job); }

void scheduler::shutdown() noexcept
{
    if (!shutdown_please.exchange(true, std::memory_order::acq_rel))
    {
        workers.shutdown();

        loop.shutdown();

        if (io.joinable()) io.join();
    }
}

void scheduler::io_loop()
{
    while (!shutdown_please.load(std::memory_order::acquire) || size() > 0)
    {
        for (auto [handle, result] : loop.dispatch())
        {
            handle.promise().return_value(result);
            workers.resume(handle);
        }
    }
}

}
