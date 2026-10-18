#include "coro/notifier.hpp"

#include <atomic>
#include <coroutine>
#include <exception>
#include <mutex>
#include <vector>

#include <spdlog/spdlog.h>

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

    if (pool->resume(woken) == woken.size()) return;

    // NOTE: a pool refuses only once it's shutting down, which it doesn't while a scheduler holds it: the scheduler
    //       waits for its tasks (which own the notifiers, e.g. HTTP/2's connections) before letting it go. So this is a
    //       bug, e.g. a pool shut down under its scheduler. Resuming the waiters here instead could deadlock
    //       (notify()'s caller may hold a lock they take), or leave them never resumed: either way, silently.
    spdlog::critical("coro::notifier: the thread pool refused to resume {} waiters (it's shutting down)", woken.size());
    std::terminate();
}

}
