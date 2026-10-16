#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <ranges>
#include <thread>
#include <type_traits>
#include <utility>
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
    // poller lets idle workers wait for I/O, rather than sleep (as Tokio's workers do): then a coroutine that I/O
    // readies continues on the worker that saw it, rather than another thread waking one to resume it. One worker polls
    // at a time, and a busy one polls (without blocking) every poll_interval jobs if no one else is.
    class poller
    {
    public:
        // poll waits for I/O (if block, until some is ready, or wake() is called), and adds the coroutines it readied
        // to ready.
        virtual void poll(bool block, std::vector<std::coroutine_handle<>>& ready) noexcept = 0;

        // wake makes a poll() that's blocked return (or the next one, if none is).
        virtual void wake() noexcept = 0;

        virtual ~poller() = default;

    protected:
        poller()                         = default;
        poller(const poller&)            = default;
        poller(poller&&)                 = default;
        poller& operator=(const poller&) = default;
        poller& operator=(poller&&)      = default;
    };

    class operation
    {
        friend class thread_pool;

        explicit operation(thread_pool* pool) noexcept
            : pool{pool}
        {}

    public:
        constexpr bool await_ready() noexcept { return false; }
        constexpr void await_resume() noexcept {}
        void           await_suspend(std::coroutine_handle<> handle) noexcept;

    private:
        thread_pool*            pool;
        std::coroutine_handle<> awaiting{nullptr};
    };

    explicit thread_pool(std::size_t concurrency = hardware_concurrency());

    thread_pool(const thread_pool&)            = delete;
    thread_pool& operator=(const thread_pool&) = delete;

    thread_pool(thread_pool&&) noexcept            = delete;
    thread_pool& operator=(thread_pool&&) noexcept = delete;

    ~thread_pool();

    // attach makes idle workers wait for I/O with p (see poller), returning false if the pool is shutting down, or
    // already has a poller.
    bool attach(poller* p) noexcept;

    // detach stops the workers polling with p, and waits for one that is to return.
    void detach(poller* p) noexcept;

    // NOTE: arguments are taken by value, as the task does not start until after the caller's full expression ends.
    //       Use std::ref() to pass by reference.
    template<typename Func, typename... Args>
        requires(std::is_invocable_v<Func, Args...>)
    auto schedule(Func func, Args... args) noexcept -> task<std::invoke_result_t<Func, Args...>>
    {
        co_await schedule();

        if constexpr (std::is_same_v<void, std::invoke_result_t<Func, Args...>>)
        {
            std::invoke(std::move(func), std::move(args)...);
            co_return;
        }
        else
        {
            co_return std::invoke(std::move(func), std::move(args)...);
        }
    }

    [[nodiscard]] operation schedule();

    // resume queues handles to be resumed, all together (so, with one lock), returning how many it queued: none if
    // the pool is shutting down.
    template<RangeOf<std::coroutine_handle<>> R>
    std::size_t resume(const R& handles) noexcept
    {
        std::size_t new_jobs = 0;
        std::size_t woken    = 0;

        {
            std::scoped_lock lock{wait_mutex};
            if (!running.load(std::memory_order::acquire)) return 0;

            for (const auto& handle : handles)
            {
                if (handle != nullptr) [[likely]]
                {
                    // NOTE: counted before it's queued, as a worker may finish it as soon as the lock is released.
                    num_jobs.fetch_add(1, std::memory_order::release);
                    jobs.emplace_back(handle);
                    ++new_jobs;
                }
            }

            woken = wake_locked(new_jobs);
        }

        notify(woken);
        return new_jobs;
    }

    bool resume(std::coroutine_handle<> handle) noexcept;

    [[nodiscard]] operation yield() { return schedule(); }
    void                    shutdown() noexcept;

    [[nodiscard]] std::size_t concurrency() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool        empty() const noexcept;
    [[nodiscard]] std::size_t queue_size() const noexcept;
    [[nodiscard]] bool        queue_empty() const noexcept;

private:
    // poll_interval is how many jobs a worker runs before polling (if no one else is), even if more are queued, so
    // that I/O isn't held up by a queue that never empties. (Tokio's event_interval.)
    static constexpr std::size_t poll_interval = 61;

    // watchdog_interval is how often the standby (see worker()) checks that someone's polling: so how long a coroutine
    // that doesn't suspend can hold up I/O, when other workers are asleep. (Go's sysmon checks at most this often, once
    // idle.)
    static constexpr std::chrono::milliseconds watchdog_interval{10};

    void worker();
    void schedule(std::coroutine_handle<> /*handle*/);

    // poll_locked polls (with wait_mutex, which lock holds, released meanwhile), and queues what's readied.
    void poll_locked(std::unique_lock<std::mutex>& lock, std::vector<std::coroutine_handle<>>& ready);

    // wake_locked wakes workers for new_jobs: sleeping workers if there are any (returning how many to notify, once
    // wait_mutex is released), or else the poller, if it's blocked. Must be called holding wait_mutex.
    std::size_t wake_locked(std::size_t new_jobs) noexcept;
    void        notify(std::size_t woken) noexcept;

    std::vector<std::thread>            threads;
    std::mutex                          wait_mutex;
    std::condition_variable             wait;
    std::deque<std::coroutine_handle<>> jobs;
    std::atomic<bool>                   running;
    std::atomic<std::size_t>            num_jobs; // queued AND currently executing

    // Guarded by wait_mutex.
    // - sleepers are the workers waiting on wait, less those already woken; wakeups are those woken that haven't
    //   woken up yet (each takes one).
    // - polling is whether a worker is polling, and poll_woken whether it has been woken (since it started).
    std::size_t             sleepers   = 0;
    std::size_t             wakeups    = 0;
    poller*                 polls      = nullptr;
    bool                    polling    = false;
    bool                    poll_woken = false;
    bool                    standby    = false; // whether a sleeping worker is the standby (see worker())
    std::condition_variable polled; // notified when a poll ends, for detach()
};

}
