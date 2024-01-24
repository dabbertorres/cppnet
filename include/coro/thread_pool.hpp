#pragma once

#include <atomic>
#include <concepts> // IWYU pragma: keep
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <ranges>
#include <thread>
#include <type_traits>
#include <vector>

#include "coro/task.hpp"

namespace net::coro
{

std::size_t hardware_concurrency(std::size_t minus_amount = 0) noexcept;

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
        constexpr bool await_ready() noexcept { return false; }
        constexpr void await_resume() noexcept {}
        void           await_suspend(std::coroutine_handle<> handle) noexcept;

    private:
        thread_pool* pool;
    };

    thread_pool(std::size_t concurrency = hardware_concurrency());

    thread_pool(const thread_pool&)            = delete;
    thread_pool& operator=(const thread_pool&) = delete;

    thread_pool(thread_pool&&) noexcept            = delete;
    thread_pool& operator=(thread_pool&&) noexcept = delete;

    ~thread_pool();

    template<typename Func, typename... Args>
        requires std::invocable<Func, Args...>
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters)
    auto schedule(Func&& func, Args&&... args) noexcept -> task<std::invoke_result_t<Func>>
    {
        co_await schedule();
        co_return std::invoke(std::forward<Func>(func), std::forward<Args>(args)...);
    }

    [[nodiscard]] operation schedule();

    template<RangeOf<std::coroutine_handle<>> R>
    void resume(const R& handles) noexcept
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

    void resume(std::coroutine_handle<> handle) noexcept;

    [[nodiscard]] operation yield();
    void                    shutdown() noexcept;

    [[nodiscard]] std::size_t concurrency() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool        empty() const noexcept;
    [[nodiscard]] std::size_t queue_size() const noexcept;
    [[nodiscard]] bool        queue_empty() const noexcept;

private:
    void worker();
    void schedule(std::coroutine_handle<>);

    std::vector<std::thread>            threads;
    std::mutex                          wait_mutex;
    std::condition_variable             wait;
    std::deque<std::coroutine_handle<>> jobs;
    std::atomic<bool>                   running;
    std::atomic<std::size_t>            num_jobs; // queued AND currently executing
};

}
