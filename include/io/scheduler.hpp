#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

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
    scheduler(std::shared_ptr<coro::thread_pool>     workers,
              const std::shared_ptr<spdlog::logger>& logger = spdlog::create<spdlog::sinks::null_sink_mt>("scheduler"));

    scheduler(const scheduler&)            = delete;
    scheduler& operator=(const scheduler&) = delete;

    scheduler(scheduler&&)            = delete;
    scheduler& operator=(scheduler&&) = delete;

    ~scheduler() noexcept;

    void register_handle(handle handle);
    void deregister_handle(handle handle);

    bool               schedule(coro::task<>&& task) noexcept;
    coro::task<result> schedule(handle handle, poll_op op, std::chrono::milliseconds timeout);
    bool               resume(std::coroutine_handle<> handle) noexcept;

    void run();

    template<typename T>
    T run_to_completion(coro::task<T>&& task)
    {
        std::optional<T> ret;

        auto wrapper = [this, task = std::move(task), &ret] -> coro::task<>
        {
            auto v = co_await task;
            shutdown();
            ret = v;
        };

        auto wrapper_task = wrapper();

        auto scheduled = schedule(std::move(wrapper_task));

        if (!scheduled)
        {
            throw 0; // TODO
        }

        running.wait(true, std::memory_order::release);

        // NOTE: will (intentionally) throw if we didn't get a value (although we should've thrown by now if we didn't)
        return ret.value();
    }

    template<>
    void run_to_completion<void>(coro::task<>&& task)
    {
        auto wrapper = [this, task = std::move(task)] -> coro::task<>
        {
            co_await task;
            shutdown();
        };

        auto wrapped_task = wrapper();

        auto scheduled = schedule(std::move(wrapped_task));

        if (!scheduled)
        {
            throw 0; // TODO
        }

        running.wait(true, std::memory_order::release);
    }

    void shutdown() noexcept;

private:
    std::shared_ptr<coro::thread_pool> workers;
    detail::event_loop                 loop;
    std::deque<coro::task<>>           tasks;
    std::mutex                         tasks_mu;
    std::atomic<bool>                  running;
};

}
