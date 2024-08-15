#include "coro/thread_pool.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace net::coro
{

std::size_t hardware_concurrency(std::size_t minus_amount) noexcept
{
    static const auto count = std::thread::hardware_concurrency();
    return count > minus_amount ? count - minus_amount : 1;
}

void thread_pool::operation::await_suspend(std::coroutine_handle<> handle) noexcept
{
    awaiting = handle;
    pool->schedule(awaiting);
}

thread_pool::thread_pool(std::size_t concurrency, std::shared_ptr<spdlog::logger> logger)
    : running(true)
    , logger{logger->clone("thread_pool")}
{
    if (concurrency == 0) concurrency = 4;

    threads.reserve(concurrency);

    for (std::size_t i = 0; i < concurrency; ++i)
    {
        threads.emplace_back(&thread_pool::worker, this);
    }
}

thread_pool::~thread_pool() { shutdown(); }

thread_pool::operation thread_pool::schedule()
{
    if (running.load(std::memory_order::relaxed)) return operation{this};

    throw std::runtime_error("thread_pool is shutting down, unable to schedule new tasks");
}

bool thread_pool::resume(std::coroutine_handle<> handle) noexcept
{
    if (handle == nullptr) return false;
    if (!running.load(std::memory_order::acquire)) return false;

    schedule(handle);
    return true;
}

void thread_pool::shutdown() noexcept
{
    if (running.exchange(false, std::memory_order::acq_rel))
    {
        logger->trace("shutting down - notifying workers...");
        wait.notify_all();

        int i = 0;
        for (auto& thread : threads)
        {
            logger->trace("shutting down - waiting on worker {}...", i);
            if (thread.joinable()) thread.join();
            ++i;
        }
    }
}

std::size_t thread_pool::concurrency() const noexcept { return threads.size(); }

std::size_t thread_pool::size() const noexcept { return num_jobs.load(std::memory_order::acquire); }
bool        thread_pool::empty() const noexcept { return size() == 0; }

std::size_t thread_pool::queue_size() const noexcept
{
    std::atomic_thread_fence(std::memory_order::acquire);
    return jobs.size();
}

bool thread_pool::queue_empty() const noexcept { return queue_size() == 0; }

void thread_pool::worker()
{
    while (running.load(std::memory_order::acquire))
    {
        std::unique_lock lock{wait_mutex};
        wait.wait(lock, [this] { return !jobs.empty() || !running.load(std::memory_order::acquire); });

        if (jobs.empty()) continue;

        auto handle = jobs.front();
        jobs.pop_front();
        lock.unlock();

        logger->trace("[c {}]: executing", handle.address());
        handle.resume();
        num_jobs.fetch_sub(1, std::memory_order::release);
        logger->trace("[c {}]: done executing; done = {}", handle.address(), handle.done());
    }

    while (num_jobs.load(std::memory_order::acquire) > 0)
    {
        std::unique_lock lock{wait_mutex};

        if (jobs.empty()) break;

        auto handle = jobs.front();
        jobs.pop_front();
        lock.unlock();

        handle.resume();
        num_jobs.fetch_sub(1, std::memory_order::release);
    }
}

void thread_pool::schedule(std::coroutine_handle<> handle)
{
    if (handle == nullptr) return;

    num_jobs.fetch_add(1, std::memory_order::release);

    {
        std::lock_guard lock{wait_mutex};
        jobs.emplace_back(handle);
        wait.notify_one();
    }
}

}
