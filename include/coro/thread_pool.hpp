#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "coro/task.hpp"

namespace net::coro
{

std::size_t hardware_concurrency(std::size_t minus_amount = 0) noexcept;

// RangeOf is a range of V (for thread_pool::resume()).
template<typename T, typename V>
concept RangeOf = std::ranges::range<T> && std::is_same_v<V, std::ranges::range_value_t<T>>;

// thread_pool runs coroutines on its worker threads. An application usually makes one, and gives it to an
// io::scheduler, which runs the application's tasks on it (see io/scheduler.hpp); schedule() moves a coroutine onto it.
//
// poller, attach(), detach() and resume() are for what readies coroutines (the scheduler), not applications.
class thread_pool
{
public:
    // poller lets idle workers wait for I/O, rather than sleep (as Tokio's workers do): then a coroutine that I/O
    // readies continues on the worker that saw it, rather than another thread waking one to resume it. One worker polls
    // at a time, and a busy one polls (without blocking) every poll_interval jobs if no one else is.
    class poller
    {
    public:
        // poll waits for I/O for up to timeout (none: until some is ready, or wake() is called; zero: not at all), and
        // adds the coroutines it readied to ready.
        virtual void poll(std::optional<std::chrono::nanoseconds> timeout,
                          std::vector<std::coroutine_handle<>>&   ready) noexcept = 0;

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
    // the pool is shutting down. From one of the pool's workers, the last of them goes in the worker's slot instead,
    // to run next on it, once what it's running returns (or suspends), never inline: so a coroutine that resumes
    // another, then waits, hands over to it without a thread waking up. (Tokio's LIFO slot, Go's runnext.) A worker
    // runs its slot at most max_next_runs times in a row before what's queued; and one that runs a coroutine which
    // doesn't return to it for long has its slot's coroutine queued for the others, by an idle worker that checks now
    // and then (see watch_slots_locked()).
    template<RangeOf<std::coroutine_handle<>> R>
    std::size_t resume(const R& handles) noexcept
    {
        auto* const             own = own_slot();
        std::coroutine_handle<> next{nullptr};
        bool                    rest = false;
        for (const auto& handle : handles)
        {
            if (handle == nullptr) [[unlikely]]
                continue;
            if (own == nullptr || next != nullptr) rest = true;
            if (own != nullptr) next = handle;
        }

        if (!rest) return own != nullptr && next != nullptr && put_next(*own, next) ? 1 : 0;

        std::size_t new_jobs  = 0;
        std::size_t displaced = 0;
        std::size_t woken     = 0;

        {
            std::scoped_lock lock{wait_mutex};
            if (!running.load(std::memory_order::acquire)) return 0;

            for (const auto& handle : handles)
            {
                if (handle != nullptr && handle != next) [[likely]]
                {
                    // NOTE: counted before it's queued, as a worker may finish it as soon as the lock is released.
                    num_jobs.fetch_add(1, std::memory_order::release);
                    jobs.emplace_back(handle);
                    ++new_jobs;
                }
            }

            if (own != nullptr && next != nullptr) displaced = put_next_locked(*own, next);
            woken = wake_locked(new_jobs + displaced);
        }

        notify(woken);
        return new_jobs + (next != nullptr ? 1 : 0);
    }

    bool resume(std::coroutine_handle<> handle) noexcept;

    [[nodiscard]] operation yield() { return schedule(); }
    void                    shutdown() noexcept;

    [[nodiscard]] std::size_t concurrency() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool        empty() const noexcept;
    [[nodiscard]] std::size_t queue_size() const noexcept;
    [[nodiscard]] bool        queue_empty() const noexcept;

    // on_worker is whether the calling thread is one of the pool's workers.
    [[nodiscard]] bool on_worker() const noexcept;

private:
    // slot is a worker's slot for the coroutine it runs next (see resume()). Only its worker puts one in, and takes it
    // out to run it; another takes it only to queue it, once it's been there too long (see watch_slots_locked()).
    struct alignas(64) slot
    {
        std::atomic<void*>         next{nullptr}; // the coroutine's address
        std::atomic<std::uint64_t> puts{0};       // how many it's been given, written before next
    };

    // seen_job is what watch_slots_locked() last saw in a slot.
    struct seen_job
    {
        void*         next = nullptr;
        std::uint64_t puts = 0;
    };

    // max_next_runs is how many times in a row a worker runs its slot's coroutine, rather than the queue's next: so
    // coroutines handing over to each other don't hold up the queue, where what I/O readied waits. (Tokio's
    // MAX_LIFO_POLLS_PER_TICK is 3: here a third run in a row slowed contended HTTP/2 request bodies, measurably.)
    static constexpr std::size_t max_next_runs = 2;

    // poll_interval is how many jobs a worker runs before polling (if no one else is), even if more are queued, so
    // that I/O isn't held up by a queue that never empties. (Tokio's event_interval.)
    static constexpr std::size_t poll_interval = 61;

    // watchdog_interval is how often the standby (see sleep_locked()) checks that someone's polling: so how long a
    // coroutine that doesn't suspend can hold up I/O, when other workers are asleep. (Go's sysmon checks at most this
    // often, once idle.) It also bounds how long a coroutine waits in the slot of a worker that doesn't come back for
    // it: from one interval to two.
    static constexpr std::chrono::milliseconds watchdog_interval{10};

    void worker(std::size_t index);
    void schedule(std::coroutine_handle<> /*handle*/);

    // own_slot is the calling thread's slot, if it's one of the pool's workers (otherwise null).
    [[nodiscard]] slot* own_slot() const noexcept;

    // put_next puts handle in own (the calling worker's slot), queuing the coroutine it held, if any; it returns false
    // if the pool is shutting down. put_next_locked does it holding wait_mutex, once running has been checked, and
    // returns how many it queued, for the caller to wake workers for.
    bool        put_next(slot& own, std::coroutine_handle<> handle) noexcept;
    std::size_t put_next_locked(slot& own, std::coroutine_handle<> handle) noexcept;

    // first_use_locked notes that a slot's been used, returning how many workers to wake for it (see slots_used).
    std::size_t first_use_locked() noexcept;

    // run_next runs own's coroutines, while there are any and runs (how many in a row) is under max_next_runs.
    void run_next(slot& own, std::size_t& runs, std::size_t& since_poll) noexcept;

    // sleep_locked sleeps (with wait_mutex, which lock holds, released meanwhile) until woken, or, as the standby, for
    // watchdog_interval.
    void sleep_locked(std::unique_lock<std::mutex>& lock);

    // poll_locked polls (with wait_mutex, which lock holds, released meanwhile), and queues what's readied.
    void poll_locked(std::unique_lock<std::mutex>& lock, std::vector<std::coroutine_handle<>>& ready);

    // watch_slots_locked queues the coroutines that have been in a slot since it last looked, at least
    // watchdog_interval ago (so those whose workers are running a coroutine that doesn't return to them), returning
    // how many. Called holding wait_mutex, by the standby and by a worker that polled.
    std::size_t watch_slots_locked() noexcept;

    // wake_locked wakes workers for new_jobs: sleeping workers if there are any (returning how many to notify, once
    // wait_mutex is released), or else the poller, if it's blocked. Must be called holding wait_mutex.
    std::size_t wake_locked(std::size_t new_jobs) noexcept;
    void        notify(std::size_t woken) noexcept;

    std::size_t                         num_workers = 0;
    std::vector<std::thread>            threads;
    std::unique_ptr<slot[]>             slots; // NOLINT(*-avoid-c-arrays): one per worker, never resized
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
    bool                    standby    = false; // whether a sleeping worker is the standby (see sleep_locked())
    std::condition_variable polled;             // notified when a poll ends, for detach()

    // Guarded by wait_mutex.
    // - poll_capped is whether the poll in progress has a timeout (watchdog_interval), as no worker is the standby.
    // - seen is what watch_slots_locked() saw in each slot when it last looked, at watched_at.
    bool                                  poll_capped = false;
    std::vector<seen_job>                 seen;
    std::chrono::steady_clock::time_point watched_at;

    // slots_used is whether any worker has used its slot: from then on, a pool without a poller has a standby too.
    std::atomic<bool> slots_used{false};
};

}
