#include "coro/notifier.hpp"

#include <atomic>
#include <coroutine>
#include <mutex>
#include <vector>

namespace net::coro
{

bool notifier::wait_operation::await_suspend(std::coroutine_handle<> awaiting)
{
    std::scoped_lock lock{n->mu};

    // NOTE: counted as waiting before checking the value, as notify() changes the value before checking for waiters
    //       (all sequentially consistent): so either this sees the change, or notify() sees this waiting, and takes
    //       the lock (after this has added itself) to wake it.
    n->waiting.fetch_add(1, std::memory_order::seq_cst);
    if (n->current.load(std::memory_order::seq_cst) != seen || n->closed())
    {
        n->waiting.fetch_sub(1, std::memory_order::relaxed);
        return false;
    }

    n->waiters.push_back(awaiting);
    return true;
}

void notifier::notify() noexcept
{
    // NOTE: see await_suspend() for why both are sequentially consistent.
    current.fetch_add(1, std::memory_order::seq_cst);
    if (waiting.load(std::memory_order::seq_cst) == 0) return;

    std::vector<std::coroutine_handle<>> woken;
    {
        std::scoped_lock lock{mu};
        woken.swap(waiters);
        waiting.store(0, std::memory_order::relaxed);
    }

    wake(woken);
}

void notifier::close() noexcept
{
    std::vector<std::coroutine_handle<>> woken;
    {
        std::scoped_lock lock{mu};
        is_closed.store(true, std::memory_order::seq_cst);
        current.fetch_add(1, std::memory_order::seq_cst);
        woken.swap(waiters);
        waiting.store(0, std::memory_order::relaxed);
    }

    wake(woken);
}

void notifier::wake(std::vector<std::coroutine_handle<>>& woken) noexcept
{
    if (woken.empty()) return;

    // NOTE: if the pool is shutting down, it resumes nothing, so they're resumed here (as they'd otherwise never be).
    if (pool != nullptr && pool->resume(woken) == woken.size()) return;
    for (auto handle : woken) handle.resume();
}

}
