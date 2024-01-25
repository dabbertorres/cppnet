#include "io/scheduler.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <utility>

#include "io/event_loop.hpp"

namespace net::io
{

scheduler::scheduler(std::size_t concurrency)
    : workers{concurrency}
    , io{&scheduler::io_loop, this}
    , shutdown_please{false}
{}

scheduler::~scheduler() noexcept { shutdown(); }

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
    while (!shutdown_please.load(std::memory_order::acquire) /* || size() > 0*/)
    {
        for (auto [handle, result] : loop.dispatch())
        {
            handle.promise().return_value(std::move(result)); // to move or not to move?
            workers.resume(handle);
        }
    }
}

}
