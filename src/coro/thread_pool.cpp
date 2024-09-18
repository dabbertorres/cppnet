#include "coro/thread_pool.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "coro/task.hpp"

namespace net::coro
{

std::size_t hardware_concurrency(std::size_t minus_amount) noexcept
{
    auto count = std::thread::hardware_concurrency();
    if (count == 0)
    {
        // TODO: get amount in OS-specific manner
    }

    return count > minus_amount ? count - minus_amount : 1;
}

void thread_pool::operation::await_suspend(std::coroutine_handle<> handle) noexcept
{
    awaiting = handle;
    pool->resume(awaiting);
}

thread_pool::thread_pool(std::size_t concurrency)
    : running(true)
{
    if (concurrency == 0) concurrency = 1;

    threads.reserve(concurrency);

    for (std::size_t i = 0; i < concurrency; ++i)
    {
        threads.emplace_back(&thread_pool::worker, this);
    }
}

thread_pool::~thread_pool() { shutdown(); }

thread_pool::operation thread_pool::schedule()
{
    if (!running.load(std::memory_order::relaxed))
        throw std::runtime_error("thread_pool is shutting down, unable to schedule new tasks");

    return operation{this};
}

bool thread_pool::resume(std::coroutine_handle<> handle) noexcept
{
    if (handle == nullptr) return false;
    if (!running.load(std::memory_order::relaxed)) return false;

    num_jobs.fetch_add(1, std::memory_order::release);

    {
        std::lock_guard lock{jobs_mutex};
        jobs.emplace_back(handle);
        jobs_available.notify_one();
    }
    return true;
}

bool thread_pool::resume(coro::task<>&& task) noexcept
{
    auto handle = std::move(task).get_handle();
    return resume(static_cast<std::coroutine_handle<>>(handle));
}

void thread_pool::shutdown() noexcept
{
    if (running.exchange(false, std::memory_order::acq_rel))
    {
        jobs_available.notify_all();

        int i = 0;
        for (auto& thread : threads)
        {
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
        std::unique_lock lock{jobs_mutex};
        jobs_available.wait(lock, [this] { return !jobs.empty() || !running.load(std::memory_order::acquire); });

        if (jobs.empty()) continue;

        auto handle = jobs.front();
        jobs.pop_front();
        lock.unlock();

        handle.resume();
        num_jobs.fetch_sub(1, std::memory_order::release);
    }

    while (num_jobs.load(std::memory_order::acquire) > 0)
    {
        std::unique_lock lock{jobs_mutex};

        if (jobs.empty()) break;

        auto handle = jobs.front();
        jobs.pop_front();
        lock.unlock();

        handle.resume();
        num_jobs.fetch_sub(1, std::memory_order::release);
    }
}

}
