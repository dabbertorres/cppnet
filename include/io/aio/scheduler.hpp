#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "coro/thread_pool.hpp"

#include "io/aio/event_loop.hpp"

namespace net::io::aio
{

using namespace std::chrono_literals;

class scheduler
{
public:
    scheduler(std::size_t concurrency = coro::hardware_concurrency());

    scheduler(const scheduler&)            = delete;
    scheduler& operator=(const scheduler&) = delete;

    scheduler(scheduler&&)            = delete;
    scheduler& operator=(scheduler&&) = delete;

    ~scheduler() noexcept;

    void resume(std::coroutine_handle<> handle);

    // TODO: consider making private...
    void process_events(std::chrono::milliseconds timeout = 0ms);

    void shutdown() noexcept;

    std::size_t size() noexcept;

private:
    void process_scheduled_tasks();
    void process_timeouts();
    void process_event(/*TODO*/);

    void io_loop();

    coro::thread_pool workers;
    std::thread       io;

    event_loop        loop;
    std::atomic<bool> shutdown_please;

    std::atomic<bool>                    schedule_triggered;
    std::mutex                           scheduled_tasks_mutex;
    std::vector<std::coroutine_handle<>> scheduled_tasks;
};

}
