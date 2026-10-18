// bench_notifier compares coro::notifier with io::scheduler::wait_on() for waking one coroutine from another, as an
// HTTP/2 connection's streams and writer would: pairs of coroutines take turns, each bumping a count the other waits
// on, with many pairs at once (as streams on many connections). A notifier has its own lock; every wait_on() and
// notify shares the scheduler's (see docs/plans/http2.md).
//
// It reports ns per hand-off (one coroutine waking the other), best of 5, by worker threads and pairs.
//
// Results when this was written (macOS, Intel Core i9-9980HK laptop, Clang 23, -O3; ns per hand-off, notifier /
// wait_on):
//
//   workers  1 pair       16 pairs     256 pairs
//   1        430 / 512    506 / 666    563 / 748
//   2        585 / 692    687 / 1082   716 / 1363
//   4        4330 / 4607  801 / 1610   792 / 1919
//   8        3117 / 2971  774 / 2084   806 / 2600
//
// With many pairs on several workers, wait_on's shared lock costs up to about 3x; a single pair on several workers is
// dominated by waking a sleeping worker either way.
#include "coro/notifier.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <print>
#include <ratio>
#include <vector>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/scheduler.hpp"

namespace
{

using net::coro::task;

constexpr int rounds = 2'000;

// side is one coroutine of a pair: the count it waits on, and how it's woken.
struct side
{
    std::atomic<std::uint64_t> count{0};
};

// with_notifier hands off through coro::notifiers.
struct with_notifier
{
    explicit with_notifier(net::coro::thread_pool* pool)
        : n{pool}
    {}

    side                s;
    net::coro::notifier n;

    void bump()
    {
        s.count.fetch_add(1, std::memory_order::release);
        n.notify();
    }

    task<> wait_for(std::uint64_t target)
    {
        while (true)
        {
            const auto seen = n.value();
            if (s.count.load(std::memory_order::acquire) >= target) co_return;
            (void)co_await n.wait(seen);
        }
    }
};

// with_wait_on hands off through the scheduler's wait_on() and notify_one().
struct with_wait_on
{
    explicit with_wait_on(net::io::scheduler* sched)
        : sched{sched}
    {}

    side                s;
    net::io::scheduler* sched;

    void bump()
    {
        s.count.fetch_add(1, std::memory_order::release);
        sched->notify_one(s.count);
    }

    task<> wait_for(std::uint64_t target)
    {
        while (true)
        {
            const auto seen = s.count.load(std::memory_order::acquire);
            if (seen >= target) co_return;
            (void)co_await sched->wait_on(s.count, seen);
        }
    }
};

template<typename Side>
task<> first(Side* mine, Side* theirs, std::latch* done)
{
    for (std::uint64_t i = 1; i <= rounds; ++i)
    {
        theirs->bump();
        co_await mine->wait_for(i);
    }
    done->count_down();
}

template<typename Side>
task<> second(Side* mine, Side* theirs, std::latch* done)
{
    for (std::uint64_t i = 1; i <= rounds; ++i)
    {
        co_await mine->wait_for(i);
        theirs->bump();
    }
    done->count_down();
}

// run runs pairs pairs of hand-offs, returning ns per hand-off.
template<typename Side, typename Make>
double run(net::io::scheduler& sched, std::size_t pairs, Make make)
{
    std::vector<std::unique_ptr<Side>> sides;
    sides.reserve(pairs * 2);
    for (std::size_t i = 0; i < pairs * 2; ++i) sides.push_back(make());

    std::latch done{static_cast<std::ptrdiff_t>(pairs * 2)};
    const auto start = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < pairs; ++i)
    {
        auto* a = sides[i * 2].get();
        auto* b = sides[(i * 2) + 1].get();
        (void)sched.spawn(second(b, a, &done));
        (void)sched.spawn(first(a, b, &done));
    }

    done.wait();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(pairs * rounds * 2);
}

template<typename Side, typename Make>
double best_of(net::io::scheduler& sched, std::size_t pairs, Make make)
{
    double best = 1e300;
    for (int i = 0; i < 5; ++i) best = std::min(best, run<Side>(sched, pairs, make));
    return best;
}

}

int main()
{
    std::println("{:>8} {:>6} {:>12} {:>12}", "workers", "pairs", "notifier", "wait_on");

    for (const std::size_t workers : {1, 2, 4, 8})
    {
        auto               pool = std::make_shared<net::coro::thread_pool>(workers);
        net::io::scheduler sched{pool};

        for (const std::size_t pairs : {1, 16, 256})
        {
            const auto notifier =
                best_of<with_notifier>(sched, pairs, [&] { return std::make_unique<with_notifier>(pool.get()); });
            const auto wait_on =
                best_of<with_wait_on>(sched, pairs, [&] { return std::make_unique<with_wait_on>(&sched); });
            std::println("{:>8} {:>6} {:>10.0f}ns {:>10.0f}ns", workers, pairs, notifier, wait_on);
        }
    }
}
