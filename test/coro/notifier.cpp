#include "coro/notifier.hpp"

#include <atomic>
#include <chrono>
#include <latch>
#include <mutex>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "coro/async_scope.hpp"
#include "coro/task.hpp"
#include "coro/thread_pool.hpp"

using net::coro::notifier;
using net::coro::task;

namespace
{

// wait_for waits until value is at least target, returning false if the notifier was closed first.
task<bool> wait_for(notifier* n, const std::atomic<int>* value, int target)
{
    while (true)
    {
        const auto seen = n->value();
        if (value->load() >= target) co_return true;
        if (!co_await n->wait(seen)) co_return false;
    }
}

}

TEST_CASE("a wait for a value that's already changed doesn't suspend", "[coro][notifier]")
{
    net::coro::thread_pool pool{1};
    notifier               n{&pool};

    const auto seen = n.value();
    n.notify();
    CHECK(n.wait(seen).await_ready());
}

TEST_CASE("notify wakes the waiters, on the pool", "[coro][notifier]")
{
    net::coro::thread_pool pool{2};
    notifier               n{&pool};
    std::atomic<int>       value{0};

    constexpr int          waiters = 8;
    std::latch             done{waiters};
    std::atomic            woken{0};
    net::coro::async_scope scope;

    for (int i = 0; i < waiters; ++i)
    {
        REQUIRE(scope.spawn(
            [](notifier* n, std::atomic<int>* value, std::atomic<int>* woken, std::latch* done) -> task<>
            {
                if (co_await wait_for(n, value, 1)) woken->fetch_add(1);
                done->count_down();
            }(&n, &value, &woken, &done)));
    }

    value.store(1);
    n.notify();
    done.wait();
    CHECK(woken.load() == waiters);
    scope.join_blocking();
}

TEST_CASE("a notify while the notifier's caller holds a lock the waiter takes doesn't deadlock", "[coro][notifier]")
{
    net::coro::thread_pool pool{2};
    notifier               n{&pool};
    std::atomic<int>       value{0};
    std::mutex             mu;
    std::latch             done{1};

    net::coro::async_scope scope;
    REQUIRE(scope.spawn(
        [](notifier* n, std::atomic<int>* value, std::mutex* mu, std::latch* done) -> task<>
        {
            co_await wait_for(n, value, 1);
            {
                // NOTE: resumed on the pool, not by notify(), whose caller holds this.
                std::scoped_lock lock{*mu};
            }
            done->count_down();
        }(&n, &value, &mu, &done)));

    // give the waiter a chance to wait (whether it has or not, it mustn't deadlock).
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    {
        std::scoped_lock lock{mu};
        value.store(1);
        n.notify();
    }

    done.wait();
    scope.join_blocking();
}

TEST_CASE("close wakes the waiters for good", "[coro][notifier]")
{
    net::coro::thread_pool pool{2};
    notifier               n{&pool};
    std::atomic<int>       value{0};
    std::latch             done{1};
    std::atomic<bool>      result{true};

    net::coro::async_scope scope;
    REQUIRE(scope.spawn(
        [](notifier* n, std::atomic<int>* value, std::atomic<bool>* result, std::latch* done) -> task<>
        {
            result->store(co_await wait_for(n, value, 1));
            done->count_down();
        }(&n, &value, &result, &done)));

    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    n.close();
    done.wait();
    CHECK_FALSE(result.load());

    // and later waits don't wait.
    CHECK(n.wait(n.value()).await_ready());
    scope.join_blocking();
}

TEST_CASE("notifiers hand off between coroutines many times without losing a wakeup", "[coro][notifier]")
{
    net::coro::thread_pool pool{4};
    notifier               to_a{&pool};
    notifier               to_b{&pool};
    std::atomic<int>       a{0};
    std::atomic<int>       b{0};

    constexpr int rounds = 20'000;
    std::latch    done{2};

    net::coro::async_scope scope;

    // a and b take turns: each bumps the other's count, and waits for its own.
    REQUIRE(scope.spawn(
        [](notifier* mine, notifier* theirs, std::atomic<int>* my, std::atomic<int>* their, std::latch* done) -> task<>
        {
            for (int i = 1; i <= rounds; ++i)
            {
                their->store(i);
                theirs->notify();
                co_await wait_for(mine, my, i);
            }
            done->count_down();
        }(&to_a, &to_b, &a, &b, &done)));

    REQUIRE(scope.spawn(
        [](notifier* mine, notifier* theirs, std::atomic<int>* my, std::atomic<int>* their, std::latch* done) -> task<>
        {
            for (int i = 1; i <= rounds; ++i)
            {
                co_await wait_for(mine, my, i);
                their->store(i);
                theirs->notify();
            }
            done->count_down();
        }(&to_b, &to_a, &b, &a, &done)));

    done.wait();
    CHECK(a.load() == rounds);
    CHECK(b.load() == rounds);
    scope.join_blocking();
}

TEST_CASE("notify and close wake every waiter, each time, however many wait", "[coro][notifier]")
{
    net::coro::thread_pool pool{2};
    notifier               n{&pool};
    std::atomic<int>       value{0};
    std::atomic<int>       woken{0};
    std::atomic<int>       closed{0};

    net::coro::async_scope scope;

    // waiter waits for value to reach target (as spawn() runs it until it waits, it's waiting once that returns).
    const auto waiter = [](notifier*               n,
                           const std::atomic<int>* value,
                           int                     target,
                           std::atomic<int>*       woken,
                           std::atomic<int>*       closed) -> task<>
    {
        if (co_await wait_for(n, value, target)) woken->fetch_add(1);
        else closed->fetch_add(1);
    };

    // one waiter, then two, then three, then more (some are held in place, the rest in a list: each round, the place
    // and the list are used again).
    int target = 0;
    for (const int waiters : {1, 2, 3, 5, 3})
    {
        ++target;
        for (int i = 0; i < waiters; ++i) REQUIRE(scope.spawn(waiter(&n, &value, target, &woken, &closed)));

        const int before = woken.load();
        value.store(target);
        n.notify();
        while (woken.load() != before + waiters) std::this_thread::yield();
    }

    // and close() wakes three waiting, for good.
    for (int i = 0; i < 3; ++i) REQUIRE(scope.spawn(waiter(&n, &value, target + 1, &woken, &closed)));
    n.close();
    scope.join_blocking();
    CHECK(closed.load() == 3);
    CHECK(woken.load() == 1 + 2 + 3 + 5 + 3);
}
