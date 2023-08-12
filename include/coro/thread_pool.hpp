#pragma once

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

#include "coro/task.hpp"

namespace net::coro
{

std::size_t hardware_concurrency() noexcept;

template<typename T, typename V>
concept RangeOf = std::ranges::range<T> && std::is_same_v<V, std::ranges::range_value_t<T>>;

class thread_pool
{
public:
    class operation
    {
        friend class thread_pool;

        explicit operation(thread_pool* pool) noexcept;

    public:
        bool await_ready() noexcept { return false; }
        void await_suspend(std::coroutine_handle<> waiting) noexcept;
        void await_resume() noexcept {}

    private:
        thread_pool*            pool;
        std::coroutine_handle<> handle;
    };

    thread_pool(std::size_t concurrency = hardware_concurrency());

    thread_pool(const thread_pool&)            = delete;
    thread_pool& operator=(const thread_pool&) = delete;

    thread_pool(thread_pool&&) noexcept            = delete;
    thread_pool& operator=(thread_pool&&) noexcept = delete;

    ~thread_pool();

    // schedule adds func to the work queue, and returns a std::future<R> which can be used
    // to retrieve the result of the job.
    // If func returns void, a std::future<void> is returned.
    template<typename Func, typename... Args>
        requires std::invocable<Func, Args...>
    auto schedule(Func&& func, Args... args) noexcept -> task<std::invoke_result_t<Func>>;

    [[nodiscard]] operation schedule();

    template<RangeOf<std::coroutine_handle<>> R>
    void resume(const R& handles) noexcept;

    void resume(std::coroutine_handle<> handle) noexcept;

    [[nodiscard]] operation yield();
    void                    shutdown() noexcept;

    std::size_t concurrency() const noexcept;
    std::size_t size() const noexcept;
    bool        empty() const noexcept;
    std::size_t queue_size() const noexcept;
    bool        queue_empty() const noexcept;

private:
    void worker();
    void schedule(std::coroutine_handle<>);

    std::vector<std::thread>            threads;
    std::mutex                          wait_mutex;
    std::condition_variable_any         wait;
    std::deque<std::coroutine_handle<>> jobs;
    std::atomic<bool>                   running;
    std::atomic<std::size_t>            num_jobs; // queued AND currently executing
};

template<typename Func, typename... Args>
    requires std::invocable<Func, Args...>
// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
auto thread_pool::schedule(Func&& func, Args... args) noexcept -> task<std::invoke_result_t<Func>>
{
    using result_type = std::invoke_result_t<Func>;

    co_await schedule();

    if constexpr (std::is_same_v<void, result_type>)
    {
        func(std::forward<Args>(args)...);
        co_return;
    }
    else
    {
        co_return func(std::forward<Args>(args)...);
    }
}

template<RangeOf<std::coroutine_handle<>> R>
void thread_pool::resume(const R& handles) noexcept
{
    std::size_t new_jobs = 0;

    {
        std::lock_guard lock{wait_mutex};
        for (const auto& handle : handles)
        {
            if (handle != nullptr) [[likely]]
            {
                jobs.emplace_back(handle);
                ++new_jobs;
            }
        }
    }

    num_jobs.fetch_add(new_jobs, std::memory_order::release);
    wait.notify_one();
}

}
