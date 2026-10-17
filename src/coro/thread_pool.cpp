#include "coro/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <mutex>
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

        // a sleeping worker can poll now, and another be the standby (see worker()): workers that fell asleep before
        // there was a poller aren't, and wouldn't check.
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

void thread_pool::worker()
{
    current_pool = this;

    std::vector<std::coroutine_handle<>> ready;
    std::size_t                          since_poll = 0;

    std::unique_lock lock{wait_mutex};

    while (running.load(std::memory_order::acquire))
    {
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

            lock.lock();
            continue;
        }

        if (can_poll)
        {
            poll_locked(lock, ready);
            since_poll = 0;
            continue;
        }

        ++sleepers;

        const auto woken     = [this] { return wakeups > 0 || !running.load(std::memory_order::acquire); };
        bool       timed_out = false;

        // One sleeping worker (the standby) wakes now and then, to check that someone's polling: no one is while the
        // worker that last polled runs a coroutine that doesn't suspend (e.g. one that blocks), even with others
        // asleep.
        if (polls != nullptr && !standby)
        {
            standby   = true;
            timed_out = !wait.wait_for(lock, watchdog_interval, woken);
            standby   = false;
        }
        else
        {
            wait.wait(lock, woken);
        }

        // NOTE: either this was woken, and takes a wakeup (perhaps another's, which then waits on, still counted), or
        //       the pool is shutting down, or it's the standby, and timed out.
        if (wakeups > 0) --wakeups;
        else --sleepers;

        // no one's polling, so this does (as it loops round), and another sleeping worker becomes the standby, in case
        // this gets stuck too. NOTE: only then, so the standby only costs a wakeup each interval.
        if (timed_out && polls != nullptr && !polling && wake_locked(1) > 0) wait.notify_one();
    }

    while (num_jobs.load(std::memory_order::acquire) > 0)
    {
        if (jobs.empty()) break;

        auto handle = jobs.front();
        jobs.pop_front();
        lock.unlock();

        handle.resume();
        num_jobs.fetch_sub(1, std::memory_order::release);

        lock.lock();
    }
}

void thread_pool::poll_locked(std::unique_lock<std::mutex>& lock, std::vector<std::coroutine_handle<>>& ready)
{
    auto* p = polls;

    // only blocks if there's nothing else to do: otherwise, it's polling as the queue hasn't emptied for a while.
    const bool block = jobs.empty();

    polling    = true;
    poll_woken = false;
    lock.unlock();

    p->poll(block, ready);

    lock.lock();
    polling = false;
    if (polls != p) polled.notify_all(); // detached meanwhile

    for (auto handle : ready)
    {
        num_jobs.fetch_add(1, std::memory_order::release);
        jobs.emplace_back(handle);
    }

    // This worker runs the first of them, so the rest are for others. NOTE: as with Tokio, no one else is woken to poll
    // while this one runs it (which would cost a wakeup per poll): a worker polls once it's idle, so a coroutine that
    // doesn't suspend for a while (e.g. blocks) holds up I/O if the other workers are asleep.
    std::size_t woken = ready.size() <= 1 ? 0 : wake_locked(ready.size() - 1);
    ready.clear();

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
    if (woken >= threads.size())
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
