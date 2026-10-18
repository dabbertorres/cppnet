#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "coro/thread_pool.hpp"

namespace net::coro
{

// notifier lets coroutines wait for something to change, futex-style: a waiter reads value(), checks its condition,
// and if it must wait, waits for the value to differ from what it read; whatever changes the condition then calls
// notify(), which changes the value and wakes the waiters. Reading the value before checking the condition means a
// notify() in between isn't lost.
//
// Unlike io::scheduler::wait_on(), it has its own lock (rather than sharing the scheduler's, which every wait_on() and
// notify takes), and no deadlines: close() wakes the waiters for good, e.g. when what they wait on has ended, and
// timeouts are for its owner to implement (by closing it, or changing the condition and notifying). Waiters are
// resumed on the pool, never by notify()'s caller, which may be holding a lock they'd take.
//
// A coroutine mustn't be destroyed while it waits, nor the notifier while any do.
class notifier
{
public:
    explicit notifier(thread_pool* pool) noexcept
        : pool{pool}
    {}

    notifier(const notifier&)            = delete;
    notifier& operator=(const notifier&) = delete;
    notifier(notifier&&)                 = delete;
    notifier& operator=(notifier&&)      = delete;
    ~notifier()                          = default;

    class wait_operation
    {
        friend class notifier;

        wait_operation(notifier* n, std::uint64_t seen) noexcept
            : n{n}
            , seen{seen}
        {}

    public:
        [[nodiscard]] bool await_ready() const noexcept { return n->value() != seen || n->closed(); }
        bool               await_suspend(std::coroutine_handle<> awaiting);

        // await_resume is false if the notifier was closed.
        [[nodiscard]] bool await_resume() const noexcept { return !n->closed(); }

    private:
        notifier*     n;
        std::uint64_t seen;
    };

    // value is what a waiter reads before checking its condition.
    [[nodiscard]] std::uint64_t value() const noexcept { return current.load(std::memory_order::acquire); }

    // wait waits until the value isn't seen (or the notifier is closed: then it resumes with false).
    [[nodiscard]] wait_operation wait(std::uint64_t seen) noexcept { return {this, seen}; }

    // notify changes the value, and wakes the waiters.
    void notify() noexcept;

    // close wakes the waiters, and makes waits from now on return false at once.
    void close() noexcept;

    [[nodiscard]] bool closed() const noexcept { return is_closed.load(std::memory_order::acquire); }

private:
    // wake resumes the waiters on the pool.
    void wake(std::vector<std::coroutine_handle<>>& woken) noexcept;

    thread_pool*               pool;
    std::atomic<std::uint64_t> current{0};
    std::atomic<bool>          is_closed{false};
    std::atomic<std::size_t>   waiting{0}; // so notify() skips the lock if no one waits

    std::mutex                           mu;
    std::vector<std::coroutine_handle<>> waiters;
};

}
