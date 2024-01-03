#include "io/aio/scheduler.hpp"

#include <coroutine>

#include "io/io.hpp"

#include "io/aio/poll.hpp"

namespace net::io::aio
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
        loop.dispatch(
            [this](std::coroutine_handle<promise> handle, result res)
            {
                handle.promise().return_value(res);
                workers.resume(handle);
            });
    }
}

}
