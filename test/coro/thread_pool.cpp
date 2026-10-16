#include "coro/thread_pool.hpp"

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <latch>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"

using net::coro::thread_pool;

namespace
{

// test_poller readies the coroutines it's given (as I/O would), and records how it's polled.
class test_poller final : public thread_pool::poller
{
public:
    void poll(bool block, std::vector<std::coroutine_handle<>>& ready) noexcept override
    {
        std::unique_lock lock{mu};
        ++polls;
        if (!block) ++non_blocking_polls;

        if (block) cv.wait(lock, [this] { return woken || !pending.empty(); });
        woken = false;

        if (!pending.empty()) readied_on = std::this_thread::get_id();
        ready.insert(ready.end(), pending.begin(), pending.end());
        pending.clear();
    }

    void wake() noexcept override
    {
        {
            std::scoped_lock lock{mu};
            woken = true;
        }
        cv.notify_all();
    }

    void ready(std::coroutine_handle<> handle)
    {
        {
            std::scoped_lock lock{mu};
            pending.push_back(handle);
        }
        cv.notify_all();
    }

    std::thread::id readied_on_thread()
    {
        std::scoped_lock lock{mu};
        return readied_on;
    }

    std::size_t non_blocking()
    {
        std::scoped_lock lock{mu};
        return non_blocking_polls;
    }

private:
    std::mutex                           mu;
    std::condition_variable              cv;
    std::vector<std::coroutine_handle<>> pending;
    bool                                 woken              = false;
    std::size_t                          polls              = 0;
    std::size_t                          non_blocking_polls = 0;
    std::thread::id                      readied_on;
};

// readied_by suspends until poller readies the coroutine.
struct readied_by
{
    test_poller* poller;

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void               await_suspend(std::coroutine_handle<> handle) const { poller->ready(handle); }
    void               await_resume() const noexcept {}
};

net::coro::task<> wait_for_poller(test_poller* poller, std::thread::id* ran_on, std::binary_semaphore* done)
{
    co_await readied_by{poller};
    *ran_on = std::this_thread::get_id();
    done->release();
}

net::coro::task<> keep_busy(thread_pool* pool, int jobs, std::binary_semaphore* done)
{
    for (int i = 0; i < jobs; ++i) co_await pool->yield();
    done->release();
}

}

TEST_CASE("shutdown does not hang", "[coro][thread_pool]")
{
    // shutdown used to race with idle workers going to sleep, leaving them waiting forever.
    for (auto i = 0; i < 1'000; ++i)
    {
        thread_pool pool{4};
    }
}

TEST_CASE("schedule copies arguments", "[coro][thread_pool]")
{
    thread_pool pool{1};

    std::binary_semaphore done{0};
    std::size_t           size = 0;

    // the temporary string is destroyed before the task runs.
    auto task = pool.schedule(
        [&](const std::string& s)
        {
            size = s.size();
            done.release();
        },
        std::string(100, 'x'));

    task.get_handle().resume();
    done.acquire();
    pool.shutdown();

    CHECK(size == 100);
}

TEST_CASE("schedule simple lambda", "[coro][thread_pool]")
{
    /* thread_pool pool; */

    /* auto future = pool.schedule([] { return true; }); */
    /* REQUIRE(future.valid()); */

    /* auto result = future.get(); */
    /* REQUIRE(result); */
}

TEST_CASE("schedule capturing lambda", "[coro][thread_pool]")
{
    /* int x = 3; */

    /* thread_pool pool; */

    /* auto future = pool.schedule([&] { return x += 5; }); */
    /* REQUIRE(future.valid()); */

    /* auto result = future.get(); */
    /* REQUIRE(result == 8); */
}

TEST_CASE("schedule functor object", "[coro][thread_pool]")
{
    /* struct doubler */
    /* { */
    /*     int x; */

    /*     int operator()() const { return x * 2; } */
    /* }; */

    /* thread_pool pool; */

    /* doubler d{.x = 7}; */

    /* auto future = pool.schedule(d); */
    /* REQUIRE(future.valid()); */

    /* auto result = future.get(); */
    /* REQUIRE(result == 14); */
}

TEST_CASE("schedule std::function", "[coro][thread_pool]")
{
    /* thread_pool pool; */

    /* int                  start  = 9; */
    /* std::function<int()> triple = [&] */
    /* { */
    /*     start *= 3; */
    /*     return start; */
    /* }; */

    /* auto future = pool.schedule(triple); */
    /* REQUIRE(future.valid()); */

    /* auto result = future.get(); */
    /* REQUIRE(result == 27); */
}

TEST_CASE("idle workers poll, and run what it readies themselves", "[coro][thread_pool][poller]")
{
    thread_pool pool{2};
    test_poller poller;
    REQUIRE(pool.attach(&poller));

    std::thread::id       ran_on;
    std::binary_semaphore done{0};
    auto                  task = wait_for_poller(&poller, &ran_on, &done);

    task.get_handle().resume();
    done.acquire();

    // NOTE: the coroutine may not have finished (and so can't be destroyed) until the workers have.
    pool.detach(&poller);
    pool.shutdown();

    CHECK(ran_on != std::this_thread::get_id());
    CHECK(ran_on == poller.readied_on_thread());
}

TEST_CASE("a pool has one poller at a time", "[coro][thread_pool][poller]")
{
    thread_pool pool{1};
    test_poller first;
    test_poller second;

    REQUIRE(pool.attach(&first));
    CHECK_FALSE(pool.attach(&second));

    // NOTE: the worker is probably blocked polling first: detach wakes it, and waits for it.
    pool.detach(&first);
    CHECK(pool.attach(&second));
    pool.detach(&second);
}

TEST_CASE("a worker polls now and then, even if the queue doesn't empty", "[coro][thread_pool][poller]")
{
    thread_pool pool{1};
    test_poller poller;
    REQUIRE(pool.attach(&poller));

    std::binary_semaphore done{0};
    auto                  task = keep_busy(&pool, 500, &done);

    task.get_handle().resume();
    done.acquire();

    // NOTE: the coroutine may not have finished (and so can't be destroyed) until the workers have.
    pool.detach(&poller);
    pool.shutdown();

    CHECK(poller.non_blocking() > 0);
}

namespace
{

// stuck_after_poll is readied by poller, and then doesn't suspend until unstuck (as a coroutine that blocks wouldn't).
net::coro::task<> stuck_after_poll(test_poller* poller, std::latch* running, std::binary_semaphore* unstick)
{
    co_await readied_by{poller};
    running->count_down();
    unstick->acquire();
}

}

TEST_CASE("a worker that's stuck running a coroutine doesn't hold up polling", "[coro][thread_pool][poller]")
{
    thread_pool pool{3};
    test_poller poller;
    REQUIRE(pool.attach(&poller));

    // the worker that polled runs the first coroutine it readied, which gets stuck.
    std::latch            running{1};
    std::binary_semaphore unstick{0};
    auto                  first = stuck_after_poll(&poller, &running, &unstick);
    first.get_handle().resume();
    running.wait();

    // so another (asleep, until it checks) must poll, to ready the next.
    std::thread::id       ran_on;
    std::binary_semaphore done{0};
    auto                  second = wait_for_poller(&poller, &ran_on, &done);

    const auto start = std::chrono::steady_clock::now();
    second.get_handle().resume();
    const bool ran     = done.try_acquire_for(std::chrono::seconds{2});
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // NOTE: before checking, so a failure doesn't leave a worker stuck.
    unstick.release();
    if (!ran) done.acquire();

    CHECK(ran);
    CHECK(elapsed < std::chrono::milliseconds{500});

    pool.detach(&poller);
    pool.shutdown();
}
