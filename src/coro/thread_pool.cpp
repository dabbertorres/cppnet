#include "coro/thread_pool.hpp"

#include <stdexcept>

namespace net::coro
{

std::size_t hardware_concurrency(std::size_t minus_amount) noexcept
{
    static const auto count = std::thread::hardware_concurrency();
    return count > 1 && count > minus_amount ? count - minus_amount : 1;
}

thread_pool::operation::operation(thread_pool* pool) noexcept
    : pool(pool)
{}

void thread_pool::operation::await_suspend(std::coroutine_handle<> waiting) noexcept
{
    handle = waiting;
    pool->schedule(handle);
}

thread_pool::thread_pool(std::size_t concurrency)
    : running(true)
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
    if (running.load(std::memory_order::relaxed))
    {
        num_jobs.fetch_sub(1, std::memory_order::release);
        return operation{this};
    }

    throw std::runtime_error("thread_pool is shutting down, unable to schedule new tasks");
}

void thread_pool::resume(std::coroutine_handle<> handle) noexcept
{
    if (handle == nullptr) return;

    num_jobs.fetch_add(1, std::memory_order::release);
    schedule(handle);
}

thread_pool::operation thread_pool::yield() { return schedule(); }

void thread_pool::shutdown() noexcept
{
    if (!running.exchange(false, std::memory_order::acq_rel))
    {
        wait.notify_all();

        for (auto& thread : threads)
        {
            if (thread.joinable()) thread.join();
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
    while (true)
    {
        std::unique_lock<std::mutex> lock(wait_mutex);
        wait.wait(lock, [&] { return !running || !jobs.empty(); });

        // TODO: complete all pending jobs before stopping?
        if (!running /*&& jobs.empty()*/) break;

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

    {
        std::lock_guard lock{wait_mutex};
        jobs.emplace_back(handle);
    }

    wait.notify_one();
}

}
