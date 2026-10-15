// bench_task compares how co_await hands over to a task: net::coro::task's handshake, and symmetric transfer (which
// it used to use, and is reproduced here). See net::coro::task's comment for why it doesn't use symmetric transfer.
//
// It times co_awaits of tasks that finish without suspending, where the handshake costs the most relative to symmetric
// transfer (an atomic exchange per co_await), both one level deep ("flat"), and two ("nested").
//
// Results when this was written (x86-64, -O2, best of 7, ns per co_await, flat / nested):
//
//   Clang 23, Linux:  symmetric 12.0 / 9.8   handshake 17.5 / 16.3
//   Clang 23, macOS:  symmetric 43.5 / 26.6  handshake 51.9 / 33.4  (macOS's allocator is slower)
//   GCC 16, Linux:    symmetric 49.0 / 24.5  handshake 60.5 / 30.7
#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <utility>
#include <vector>

#include "coro/task.hpp"

namespace
{

// symmetric_task is a minimal lazy task that uses symmetric transfer: await_suspend returns the task's handle, and its
// final_suspend returns the awaiting coroutine's.
template<typename T>
class [[nodiscard]] symmetric_task
{
public:
    struct promise_type
    {
        std::optional<T>        value;
        std::exception_ptr      error;
        std::coroutine_handle<> continuation{std::noop_coroutine()};

        symmetric_task get_return_object() noexcept
        {
            return symmetric_task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        auto final_suspend() noexcept
        {
            struct final_awaitable
            {
                [[nodiscard]] bool      await_ready() const noexcept { return false; }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> self) noexcept
                {
                    return self.promise().continuation;
                }
                void await_resume() noexcept {}
            };

            return final_awaitable{};
        }

        void return_value(T result) { value = std::move(result); }
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };

    explicit symmetric_task(std::coroutine_handle<promise_type> handle) noexcept
        : handle{handle}
    {}

    symmetric_task(symmetric_task&& other) noexcept
        : handle{std::exchange(other.handle, nullptr)}
    {}

    symmetric_task(const symmetric_task&)            = delete;
    symmetric_task& operator=(const symmetric_task&) = delete;
    symmetric_task& operator=(symmetric_task&&)      = delete;

    ~symmetric_task()
    {
        if (handle) handle.destroy();
    }

    auto operator co_await() && noexcept
    {
        struct awaitable
        {
            std::coroutine_handle<promise_type> handle;

            [[nodiscard]] bool      await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
            {
                handle.promise().continuation = continuation;
                return handle;
            }
            T await_resume()
            {
                if (handle.promise().error) std::rethrow_exception(handle.promise().error);
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the task either set it, or threw.
                return std::move(*handle.promise().value);
            }
        };

        return awaitable{handle};
    }

    // run runs the task, which must finish without suspending, and returns its result.
    T run()
    {
        handle.resume();
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the task must have finished, and set it.
        return std::move(*handle.promise().value);
    }

private:
    std::coroutine_handle<promise_type> handle;
};

// The same coroutines, for each kind of task.

template<template<typename> typename Task>
Task<long> leaf(long i)
{
    // NOTE: depends on i, so it isn't computed in advance.
    co_return i & 1;
}

template<template<typename> typename Task>
Task<long> middle(long i)
{
    co_return co_await leaf<Task>(i) + 1;
}

template<template<typename> typename Task>
Task<long> loop(long n, bool nested)
{
    long total = 0;
    for (long i = 0; i < n; ++i) total += nested ? co_await middle<Task>(i) : co_await leaf<Task>(i);
    co_return total;
}

template<typename T>
using handshake_task = net::coro::task<T>;

long run(handshake_task<long> task)
{
    auto handle = task.get_handle();
    handle.resume();
    return std::move(task).get_promise().result();
}

long run(symmetric_task<long> task) { return task.run(); }

constexpr long iterations = 20'000'000;
constexpr int  repeats    = 7;

template<template<typename> typename Task>
void measure(const char* name)
{
    for (bool nested : {false, true})
    {
        std::vector<double> runs;

        for (int i = 0; i < repeats; ++i)
        {
            auto start = std::chrono::steady_clock::now();
            auto total = run(loop<Task>(iterations, nested));
            auto took  = std::chrono::steady_clock::now() - start;

            // the result is checked, so the work can't be skipped.
            const long expect = nested ? iterations + iterations / 2 : iterations / 2;
            if (total != expect)
            {
                std::fprintf(stderr, "%s: wrong result %ld, expected %ld\n", name, total, expect);
                std::exit(1);
            }

            const double awaits = nested ? 2.0 * iterations : iterations;
            runs.push_back(std::chrono::duration<double, std::nano>(took).count() / awaits);
        }

        std::ranges::sort(runs);
        std::printf("%-10s %-7s best %6.2f ns   median %6.2f ns   per co_await\n",
                    name,
                    nested ? "nested" : "flat",
                    runs.front(),
                    runs[runs.size() / 2]);
    }
}

}

int main()
{
#ifndef __OPTIMIZE__
    // besides not meaning much, symmetric transfer can overflow the stack here (with GCC).
    std::fprintf(stderr, "bench_task needs an optimized build (e.g. --config Release)\n");
    return 1;
#else
    measure<symmetric_task>("symmetric");
    measure<handshake_task>("handshake");
    return 0;
#endif
}
