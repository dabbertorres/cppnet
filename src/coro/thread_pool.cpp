#include "coro/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace net::coro
{

namespace
{

    // current_pool is the pool whose worker the thread is, if any. (Set by the workers, so not const.)
    // NOLINTNEXTLINE(*-avoid-non-const-global-variables)
    thread_local const thread_pool* current_pool = nullptr;

    // current_slot is the slot of the worker the thread is, if any (a thread_pool::slot, which is private).
    // NOLINTNEXTLINE(*-avoid-non-const-global-variables)
    thread_local void* current_slot = nullptr;

}

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

thread_pool::thread_pool(std::size_t concurrency)
    : running(true)
{
    if (concurrency == 0) concurrency = 4;

    num_workers = concurrency;
    slots       = std::make_unique<slot[]>(concurrency); // NOLINT(*-avoid-c-arrays)
    seen.resize(concurrency);
    watched_at = std::chrono::steady_clock::now();
    threads.reserve(concurrency);

    for (std::size_t i = 0; i < concurrency; ++i)
    {
        threads.emplace_back(&thread_pool::worker, this, i);
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
    if (auto* own = own_slot(); own != nullptr) return put_next(*own, handle);
    if (!running.load(std::memory_order::acquire)) return false;

    schedule(handle);
    return true;
}

thread_pool::slot* thread_pool::own_slot() const noexcept
{
    return current_pool == this ? static_cast<slot*>(current_slot) : nullptr;
}

bool thread_pool::put_next(slot& own, std::coroutine_handle<> handle) noexcept
{
    // NOTE: without the lock: if the pool starts shutting down after this looks, this worker runs it before it stops
    //       (see worker()), as it would a queued one.
    if (!running.load(std::memory_order::acquire)) return false;

    num_jobs.fetch_add(1, std::memory_order::release);
    own.puts.store(own.puts.load(std::memory_order::relaxed) + 1, std::memory_order::relaxed);
    auto* const displaced = own.next.exchange(handle.address(), std::memory_order::acq_rel);
    if (displaced == nullptr && slots_used.load(std::memory_order::relaxed)) return true;

    std::size_t woken = 0;
    {
        std::scoped_lock lock{wait_mutex};
        if (displaced != nullptr) jobs.emplace_back(std::coroutine_handle<>::from_address(displaced));
        woken = wake_locked((displaced != nullptr ? 1 : 0) + first_use_locked());
    }

    notify(woken);
    return true;
}

std::size_t thread_pool::put_next_locked(slot& own, std::coroutine_handle<> handle) noexcept
{
    num_jobs.fetch_add(1, std::memory_order::release);
    own.puts.store(own.puts.load(std::memory_order::relaxed) + 1, std::memory_order::relaxed);
    auto* const displaced = own.next.exchange(handle.address(), std::memory_order::acq_rel);
    if (displaced != nullptr) jobs.emplace_back(std::coroutine_handle<>::from_address(displaced));

    return (displaced != nullptr ? 1 : 0) + first_use_locked();
}

std::size_t thread_pool::first_use_locked() noexcept
{
    if (slots_used.exchange(true, std::memory_order::relaxed)) return 0;

    // a pool without a poller has had no standby: a sleeping worker wakes to be it (finding nothing to do).
    return polls == nullptr && !standby && num_workers > 1 ? 1 : 0;
}

void thread_pool::run_next(slot& own, std::size_t& runs, std::size_t& since_poll) noexcept
{
    while (runs < max_next_runs && own.next.load(std::memory_order::relaxed) != nullptr)
    {
        // NOTE: null if watch_slots_locked() has just queued it.
        auto* const next = own.next.exchange(nullptr, std::memory_order::acq_rel);
        if (next == nullptr) return;

        ++runs;
        std::coroutine_handle<>::from_address(next).resume();
        num_jobs.fetch_sub(1, std::memory_order::release);
        ++since_poll;
    }
}

std::size_t thread_pool::watch_slots_locked() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now - watched_at < watchdog_interval) return 0;
    watched_at = now;

    std::size_t moved = 0;
    for (std::size_t i = 0; i < num_workers; ++i)
    {
        auto& own  = slots[i];
        auto& last = seen[i];
        auto* next = own.next.load(std::memory_order::acquire);
        // NOTE: puts is written before next, so it's at least the count of the put that left next there.
        const auto puts = own.puts.load(std::memory_order::relaxed);

        // the same put as last time: its worker hasn't come back for it since.
        if (next != nullptr && next == last.next && puts == last.puts
            && own.next.compare_exchange_strong(next, nullptr, std::memory_order::acq_rel))
        {
            jobs.emplace_back(std::coroutine_handle<>::from_address(next));
            ++moved;
            last = {};
            continue;
        }

        last = {.next = next, .puts = puts};
    }

    return moved;
}

void thread_pool::shutdown() noexcept
{
    {
        // must be modified under the lock, otherwise a worker can check the predicate, miss the notify, and sleep
        // forever.
        std::scoped_lock lock{wait_mutex};
        if (!running.exchange(false, std::memory_order::acq_rel)) return;

        if (polling && polls != nullptr) polls->wake();
    }

    wait.notify_all();

    for (auto& thread : threads)
    {
        if (thread.joinable()) thread.join();
    }
}

bool thread_pool::attach(poller* p) noexcept
{
    std::size_t woken = 0;

    {
        std::scoped_lock lock{wait_mutex};
        if (!running.load(std::memory_order::acquire) || polls != nullptr) return false;

        polls = p;

        // a sleeping worker can poll now, and another be the standby (see sleep_locked()): workers that fell asleep
        // before there was a poller aren't, and wouldn't check.
        woken = wake_locked(2);
    }

    notify(woken);
    return true;
}

void thread_pool::detach(poller* p) noexcept
{
    std::unique_lock lock{wait_mutex};
    if (polls != p) return;

    polls = nullptr;
    while (polling)
    {
        p->wake();
        polled.wait(lock);
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

bool thread_pool::on_worker() const noexcept { return current_pool == this; }

void thread_pool::worker(std::size_t index)
{
    auto& own    = slots[index];
    current_pool = this;
    current_slot = &own;

    std::vector<std::coroutine_handle<>> ready;
    std::size_t                          since_poll = 0;
    std::size_t                          runs       = 0; // of its slot's coroutines, in a row

    std::unique_lock lock{wait_mutex};

    while (running.load(std::memory_order::acquire))
    {
        // its slot's coroutine first, unless it's had its turns in a row: then it's queued, behind what's waiting.
        if (own.next.load(std::memory_order::relaxed) != nullptr)
        {
            if (runs < max_next_runs)
            {
                lock.unlock();
                run_next(own, runs, since_poll);
                lock.lock();
                continue;
            }

            if (auto* next = own.next.exchange(nullptr, std::memory_order::acq_rel); next != nullptr)
                jobs.emplace_back(std::coroutine_handle<>::from_address(next));
        }

        // runs the next job, unless it's time to poll (and no one else is).
        const bool can_poll = polls != nullptr && !polling;
        if (!jobs.empty() && (since_poll < poll_interval || !can_poll))
        {
            auto handle = jobs.front();
            jobs.pop_front();
            lock.unlock();

            handle.resume();
            num_jobs.fetch_sub(1, std::memory_order::release);
            ++since_poll;

            // then what it resumed last, without the lock.
            runs = 0;
            run_next(own, runs, since_poll);

            lock.lock();
            continue;
        }

        if (can_poll)
        {
            poll_locked(lock, ready);
            since_poll = 0;
            continue;
        }

        sleep_locked(lock);
    }

    // NOTE: what's left in its slot is run as what's queued is, as it's this worker's alone.
    while (true)
    {
        if (auto* next = own.next.exchange(nullptr, std::memory_order::acq_rel); next != nullptr)
            jobs.emplace_back(std::coroutine_handle<>::from_address(next));
        if (num_jobs.load(std::memory_order::acquire) == 0 || jobs.empty()) break;

        auto handle = jobs.front();
        jobs.pop_front();
        lock.unlock();

        handle.resume();
        num_jobs.fetch_sub(1, std::memory_order::release);

        lock.lock();
    }
}

void thread_pool::sleep_locked(std::unique_lock<std::mutex>& lock)
{
    ++sleepers;

    const auto woken     = [this] { return wakeups > 0 || !running.load(std::memory_order::acquire); };
    bool       timed_out = false;

    // One sleeping worker (the standby) wakes now and then, to check that someone's polling (no one is while the worker
    // that last polled runs a coroutine that doesn't suspend, e.g. one that blocks, even with others asleep), and to
    // queue what's been held up in a slot (see watch_slots_locked()).
    if ((polls != nullptr || (num_workers > 1 && slots_used.load(std::memory_order::relaxed))) && !standby)
    {
        standby   = true;
        timed_out = !wait.wait_for(lock, watchdog_interval, woken);
        standby   = false;

        // a poll that has no timeout, as this was the standby, gets one (see poll_locked()), to watch the slots.
        if (!timed_out && polling && !poll_capped && !poll_woken && polls != nullptr)
        {
            poll_woken = true;
            polls->wake();
        }
    }
    else
    {
        wait.wait(lock, woken);
    }

    // NOTE: either this was woken, and takes a wakeup (perhaps another's, which then waits on, still counted), or the
    //       pool is shutting down, or it's the standby, and timed out.
    if (wakeups > 0) --wakeups;
    else --sleepers;

    if (!timed_out) return;

    // this runs the first of what it queued, and others are woken for the rest.
    if (const auto moved = watch_slots_locked(); moved > 1) notify(wake_locked(moved - 1));

    // no one's polling, so this does (as its worker loops round), and another sleeping worker becomes the standby, in
    // case this gets stuck too. NOTE: only then, so the standby only costs a wakeup each interval.
    if (polls != nullptr && !polling && wake_locked(1) > 0) wait.notify_one();
}

void thread_pool::poll_locked(std::unique_lock<std::mutex>& lock, std::vector<std::coroutine_handle<>>& ready)
{
    auto* p = polls;

    // only blocks if there's nothing else to do: otherwise, it's polling as the queue hasn't emptied for a while. With
    // no standby, it stops now and then to watch the slots in its place.
    std::optional<std::chrono::nanoseconds> timeout = std::chrono::nanoseconds::zero();
    poll_capped                                     = jobs.empty() && num_workers > 1 && !standby;
    if (jobs.empty()) timeout.reset();
    if (poll_capped) timeout = watchdog_interval;

    polling    = true;
    poll_woken = false;
    lock.unlock();

    p->poll(timeout, ready);

    lock.lock();
    polling     = false;
    poll_capped = false;
    if (polls != p) polled.notify_all(); // detached meanwhile

    for (auto handle : ready)
    {
        num_jobs.fetch_add(1, std::memory_order::release);
        jobs.emplace_back(handle);
    }

    const auto readied = ready.size() + (num_workers > 1 ? watch_slots_locked() : 0);
    ready.clear();

    // This worker runs the first of them, so the rest are for others. NOTE: as with Tokio, no one else is woken to poll
    // while this one runs it (which would cost a wakeup per poll): a worker polls once it's idle, so a coroutine that
    // doesn't suspend for a while (e.g. blocks) holds up I/O if the other workers are asleep.
    std::size_t woken = readied <= 1 ? 0 : wake_locked(readied - 1);

    if (woken > 0)
    {
        lock.unlock();
        notify(woken);
        lock.lock();
    }
}

std::size_t thread_pool::wake_locked(std::size_t new_jobs) noexcept
{
    const auto woken = std::min(new_jobs, sleepers);
    sleepers -= woken;
    wakeups += woken;

    // a worker blocked polling is idle too.
    if (woken < new_jobs && polling && !poll_woken && polls != nullptr)
    {
        poll_woken = true;
        polls->wake();
    }

    return woken;
}

void thread_pool::notify(std::size_t woken) noexcept
{
    if (woken >= num_workers)
    {
        wait.notify_all();
        return;
    }

    for (std::size_t i = 0; i < woken; ++i) wait.notify_one();
}

void thread_pool::schedule(std::coroutine_handle<> handle)
{
    if (handle == nullptr) return;

    std::size_t woken = 0;

    {
        std::scoped_lock lock{wait_mutex};
        num_jobs.fetch_add(1, std::memory_order::release);
        jobs.emplace_back(handle);
        woken = wake_locked(1);
    }

    notify(woken);
}

}
