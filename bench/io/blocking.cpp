// bench_blocking measures io::scheduler::run_blocking(): a call's round trip (its hop to a blocking thread, func, and
// back to the workers), and the heap allocations it makes, awaited by one task at a time, and by several at once (as
// the system resolver's lookups are), with and without a stop token.
//
// It reports ns and allocations per call, best of 5 (the allocations of the fastest run, on any thread).
//
// Results when this was written (macOS, Intel Core i9-9980HK laptop, Clang 23, -O3, shared with other work; medians
// of 5 runs, ns per call, without / with a stop token), when run_blocking() became an awaitable whose state outlives
// its caller, against the coroutine it was before:
//
//   workers  callers  coroutine     state of its own
//   1        1        7837 / 7836   7950 / 8066
//   1        8        2540 / 2528   2402 / 2652
//   4        1        7409 / 7538   8289 / 8303
//   4        8        1908 / 1902   1998 / 2016
//
// One allocation per call either way (the coroutine's frame, then the state's). The times are within the machine's
// noise (runs of either varied from about 7.2 to 9 us for one caller): a call is two hand-overs between threads, each
// waking a sleeping one.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <latch>
#include <limits>
#include <memory>
#include <new>
#include <print>
#include <ratio>
#include <stop_token>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/scheduler.hpp"

// NOLINTBEGIN(misc-new-delete-overloads,cppcoreguidelines-no-malloc,cppcoreguidelines-avoid-non-const-global-variables,readability-inconsistent-declaration-parameter-name):
// counting allocations, by replacing these.
namespace
{

// the heap allocations so far, on any thread.
std::atomic<std::size_t> allocations = 0;

}

void* operator new(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order::relaxed);
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t /*size*/) noexcept { std::free(p); }
// NOLINTEND(misc-new-delete-overloads,cppcoreguidelines-no-malloc,cppcoreguidelines-avoid-non-const-global-variables,readability-inconsistent-declaration-parameter-name)

namespace
{

using net::coro::task;

constexpr int calls = 20'000;

// caller makes calls run_blocking() calls one after another, each func returning a number, with a stop token if
// stoppable.
task<> caller(net::io::scheduler* sched, bool stoppable, std::atomic<std::int64_t>* sum, std::latch* done)
{
    std::int64_t total = 0;
    for (int i = 0; i < calls; ++i)
    {
        if (stoppable)
        {
            auto ran = co_await sched->run_blocking([i] { return i; }, std::stop_token{});
            total += *ran;
        }
        else
        {
            total += co_await sched->run_blocking([i] { return i; });
        }
    }
    sum->fetch_add(total, std::memory_order::relaxed);
    done->count_down();
}

struct result
{
    double ns          = std::numeric_limits<double>::max();
    double allocations = 0;
};

// run runs callers tasks at once, returning ns and allocations per call.
result run(net::io::scheduler& sched, std::size_t callers, bool stoppable)
{
    std::atomic<std::int64_t> sum{0};
    std::latch                done{static_cast<std::ptrdiff_t>(callers)};

    const auto before = allocations.load(std::memory_order::relaxed);
    const auto start  = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < callers; ++i) (void)sched.spawn(caller(&sched, stoppable, &sum, &done));

    done.wait();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto made    = allocations.load(std::memory_order::relaxed) - before;
    const auto total   = static_cast<double>(callers * calls);

    return {.ns          = std::chrono::duration<double, std::nano>(elapsed).count() / total,
            .allocations = static_cast<double>(made) / total};
}

result best_of(net::io::scheduler& sched, std::size_t callers, bool stoppable)
{
    result best;
    for (int i = 0; i < 5; ++i)
    {
        auto r = run(sched, callers, stoppable);
        if (r.ns < best.ns) best = r;
    }
    return best;
}

}

int main()
{
    std::println("{:>8} {:>8} {:>10} {:>12} {:>12}", "workers", "callers", "stop", "ns/call", "allocs/call");

    for (const std::size_t workers : {1, 4})
    {
        auto               pool = std::make_shared<net::coro::thread_pool>(workers);
        net::io::scheduler sched{pool};

        for (const std::size_t callers : {1, 8})
        {
            for (const bool stoppable : {false, true})
            {
                const auto r = best_of(sched, callers, stoppable);
                std::println("{:>8} {:>8} {:>10} {:>12.0f} {:>12.2f}",
                             workers,
                             callers,
                             stoppable ? "token" : "none",
                             r.ns,
                             r.allocations);
            }
        }

        sched.shutdown();
    }
}
