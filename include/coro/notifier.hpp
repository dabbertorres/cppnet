#pragma once

#include <array>
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
// A coroutine mustn't be destroyed while it waits, nor the notifier while any do. The pool must be running whenever
// waiters are woken: if it refuses them (it's shutting down), the program terminates, as resuming them on the
// notifier's caller could deadlock.
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
    // How many waiters are held in place, rather than in a list on the heap: so a notifier with few waiters (e.g. an
    // HTTP/2 stream's reader and writer) doesn't allocate each time one waits.
    static constexpr std::size_t inline_waiters = 2;

    // woken_list is the waiters notify() or close() took, to resume once the lock's released.
    struct woken_list
    {
        std::array<std::coroutine_handle<>, inline_waiters> first{};
        std::size_t                                         count = 0; // of first
        std::vector<std::coroutine_handle<>>                rest;
    };

    // take_waiters moves the waiters into woken (holding mu).
    void take_waiters(woken_list& woken) noexcept;

    // wake resumes the waiters on the pool.
    void wake(woken_list& woken) noexcept;

    thread_pool*               pool;
    std::atomic<std::uint64_t> current{0};
    std::atomic<bool>          is_closed{false};
    std::atomic<std::size_t>   waiting{0}; // so notify() skips the lock if no one waits

    std::mutex                                          mu;
    std::array<std::coroutine_handle<>, inline_waiters> first_waiters{};
    std::size_t                                         first_count = 0; // of first_waiters
    std::vector<std::coroutine_handle<>>                waiters;         // those past first_waiters
};

}
