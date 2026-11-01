#include "coro/thread_pool.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <latch>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
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
    void poll(std::optional<std::chrono::nanoseconds> timeout,
              std::vector<std::coroutine_handle<>>&   ready) noexcept override
    {
        std::unique_lock lock{mu};
        ++polls;

        const auto readied = [this] { return woken || !pending.empty(); };
        if (!timeout) cv.wait(lock, readied);
        else if (*timeout > std::chrono::nanoseconds::zero()) cv.wait_for(lock, *timeout, readied);
        else ++non_blocking_polls;
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

    // NOTE: the workers fall asleep before the poller's attached, as a scheduler's usually do. Timed, as nothing says
    //       when they have: if they haven't, the test passes without starting from there.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
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

namespace
{

// noted records id (on the pool's worker), then resumes next on the pool, if there is one, and releases done, if given.
net::coro::task<> noted(std::vector<int>*       order,
                        int                     id,
                        thread_pool*            pool = nullptr,
                        std::coroutine_handle<> next = nullptr,
                        std::binary_semaphore*  done = nullptr)
{
    order->push_back(id);
    if (pool != nullptr && next != nullptr) pool->resume(next);
    if (done != nullptr) done->release();
    co_return;
}

// handle_of is a lazy task's coroutine, for the pool to start.
std::coroutine_handle<> handle_of(net::coro::task<>& task) { return task.get_handle(); }

}

TEST_CASE("a coroutine a worker resumes runs next on it, before what's queued", "[coro][thread_pool][slot]")
{
    thread_pool pool{1};

    std::vector<int>      order;
    std::binary_semaphore queue_it{0};
    std::binary_semaphore queued{0};
    std::binary_semaphore done{0};

    auto first  = noted(&order, 1);
    auto second = noted(&order, 2, nullptr, nullptr, &done);
    auto next   = noted(&order, 3);

    // on the worker: first is queued (by another thread, while this waits), then next is resumed.
    auto drive = [](thread_pool*            pool,
                    std::binary_semaphore*  queue_it,
                    std::binary_semaphore*  queued,
                    std::coroutine_handle<> next) -> net::coro::task<>
    {
        co_await pool->schedule();
        queue_it->release();
        queued->acquire();
        pool->resume(next);
    }(&pool, &queue_it, &queued, handle_of(next));
    drive.get_handle().resume();

    // NOTE: released whatever happens, so the worker isn't left waiting.
    const bool started = queue_it.try_acquire_for(std::chrono::seconds{10});
    if (started)
    {
        CHECK(pool.resume(handle_of(first)));
        CHECK(pool.resume(handle_of(second)));
    }
    queued.release();

    const bool finished = started && done.try_acquire_for(std::chrono::seconds{10});
    pool.shutdown();

    REQUIRE(finished);
    CHECK(order == std::vector{3, 1, 2});
}

TEST_CASE("a worker resuming several coroutines runs the last next, and queues the rest", "[coro][thread_pool][slot]")
{
    thread_pool pool{1};

    std::vector<int>      order;
    std::binary_semaphore done{0};

    auto first  = noted(&order, 1);
    auto second = noted(&order, 2, nullptr, nullptr, &done);
    auto third  = noted(&order, 3);

    auto drive = [](thread_pool* pool, std::array<std::coroutine_handle<>, 3> handles) -> net::coro::task<>
    {
        co_await pool->schedule();
        pool->resume(handles);
    }(&pool, {handle_of(first), handle_of(second), handle_of(third)});
    drive.get_handle().resume();

    const bool finished = done.try_acquire_for(std::chrono::seconds{10});
    pool.shutdown();

    REQUIRE(finished);
    CHECK(order == std::vector{3, 1, 2});
}

TEST_CASE("a coroutine displaced from a worker's slot is queued, and runs", "[coro][thread_pool][slot]")
{
    SECTION("after the one that took its place")
    {
        thread_pool pool{1};

        std::vector<int>      order;
        std::binary_semaphore done{0};

        auto displaced = noted(&order, 1, nullptr, nullptr, &done);
        auto next      = noted(&order, 2);

        auto drive =
            [](thread_pool* pool, std::coroutine_handle<> displaced, std::coroutine_handle<> next) -> net::coro::task<>
        {
            co_await pool->schedule();
            pool->resume(displaced);
            pool->resume(next);
        }(&pool, handle_of(displaced), handle_of(next));
        drive.get_handle().resume();

        const bool finished = done.try_acquire_for(std::chrono::seconds{10});
        pool.shutdown();

        REQUIRE(finished);
        CHECK(order == std::vector{2, 1});
    }

    SECTION("on another worker, while its own is busy")
    {
        thread_pool pool{2};

        std::vector<int>      order;
        std::binary_semaphore ran{0};
        std::binary_semaphore done{0};
        std::atomic<bool>     displaced_ran = false;

        auto displaced = noted(&order, 1, nullptr, nullptr, &ran);
        auto next      = noted(&order, 2, nullptr, nullptr, &done);

        // the worker that resumed them waits for the displaced one, which only another worker can run.
        auto drive = [](thread_pool*            pool,
                        std::coroutine_handle<> displaced,
                        std::coroutine_handle<> next,
                        std::binary_semaphore*  ran,
                        std::atomic<bool>*      displaced_ran) -> net::coro::task<>
        {
            co_await pool->schedule();
            pool->resume(displaced);
            pool->resume(next);
            displaced_ran->store(ran->try_acquire_for(std::chrono::seconds{10}));
        }(&pool, handle_of(displaced), handle_of(next), &ran, &displaced_ran);
        drive.get_handle().resume();

        const bool finished = done.try_acquire_for(std::chrono::seconds{20});
        pool.shutdown();

        REQUIRE(finished);
        CHECK(displaced_ran.load());
        CHECK(order == std::vector{1, 2});
    }
}

TEST_CASE("a worker runs its slot at most twice in a row, then what's queued", "[coro][thread_pool][slot]")
{
    thread_pool pool{1};

    std::vector<int>      order;
    std::binary_semaphore queue_it{0};
    std::binary_semaphore queued{0};
    std::binary_semaphore done{0};

    // a chain, each resuming the next: 1 and 2 run in the slot, then 3 is queued, behind 0; then 4 and 5 in the slot.
    auto c5     = noted(&order, 5, nullptr, nullptr, &done);
    auto c4     = noted(&order, 4, &pool, handle_of(c5));
    auto c3     = noted(&order, 3, &pool, handle_of(c4));
    auto c2     = noted(&order, 2, &pool, handle_of(c3));
    auto c1     = noted(&order, 1, &pool, handle_of(c2));
    auto queue0 = noted(&order, 0);

    auto drive = [](thread_pool*            pool,
                    std::binary_semaphore*  queue_it,
                    std::binary_semaphore*  queued,
                    std::coroutine_handle<> first) -> net::coro::task<>
    {
        co_await pool->schedule();
        queue_it->release();
        queued->acquire();
        pool->resume(first);
    }(&pool, &queue_it, &queued, handle_of(c1));
    drive.get_handle().resume();

    // NOTE: released whatever happens, so the worker isn't left waiting.
    const bool started = queue_it.try_acquire_for(std::chrono::seconds{10});
    if (started) CHECK(pool.resume(handle_of(queue0)));
    queued.release();

    const bool finished = started && done.try_acquire_for(std::chrono::seconds{10});
    pool.shutdown();

    REQUIRE(finished);
    CHECK(order == std::vector{1, 2, 0, 3, 4, 5});
}

TEST_CASE("resumes from other threads, and yields, are queued", "[coro][thread_pool][slot]")
{
    SECTION("resumed by another thread: in the order they came")
    {
        thread_pool pool{1};

        std::vector<int>      order;
        std::binary_semaphore busy{0};
        std::binary_semaphore go_on{0};
        std::binary_semaphore done{0};

        auto first  = noted(&order, 1);
        auto second = noted(&order, 2, nullptr, nullptr, &done);

        // the worker's held up, so both wait.
        auto hold =
            [](thread_pool* pool, std::binary_semaphore* busy, std::binary_semaphore* go_on) -> net::coro::task<>
        {
            co_await pool->schedule();
            busy->release();
            go_on->acquire();
        }(&pool, &busy, &go_on);
        hold.get_handle().resume();

        // NOTE: released whatever happens, so the worker isn't left waiting.
        const bool started = busy.try_acquire_for(std::chrono::seconds{10});
        if (started)
        {
            CHECK(pool.resume(handle_of(first)));
            CHECK(pool.resume(handle_of(second)));
        }
        go_on.release();

        const bool finished = started && done.try_acquire_for(std::chrono::seconds{10});
        pool.shutdown();

        REQUIRE(finished);
        CHECK(order == std::vector{1, 2});
    }

    SECTION("a yield: behind what's queued, and its worker's slot")
    {
        thread_pool pool{1};

        std::vector<int>      order;
        std::binary_semaphore queue_it{0};
        std::binary_semaphore queued{0};
        std::binary_semaphore done{0};

        auto queued1 = noted(&order, 1);
        auto next    = noted(&order, 2);

        auto drive = [](thread_pool*            pool,
                        std::vector<int>*       order,
                        std::binary_semaphore*  queue_it,
                        std::binary_semaphore*  queued,
                        std::coroutine_handle<> next,
                        std::binary_semaphore*  done) -> net::coro::task<>
        {
            co_await pool->schedule();
            queue_it->release();
            queued->acquire();
            pool->resume(next);
            co_await pool->yield();
            order->push_back(3);
            done->release();
        }(&pool, &order, &queue_it, &queued, handle_of(next), &done);
        drive.get_handle().resume();

        // NOTE: released whatever happens, so the worker isn't left waiting.
        const bool started = queue_it.try_acquire_for(std::chrono::seconds{10});
        if (started) CHECK(pool.resume(handle_of(queued1)));
        queued.release();

        const bool finished = started && done.try_acquire_for(std::chrono::seconds{10});
        pool.shutdown();

        REQUIRE(finished);
        CHECK(order == std::vector{2, 1, 3});
    }
}

TEST_CASE("a pool shutting down runs what's in its workers' slots", "[coro][thread_pool][slot]")
{
    std::vector<int>      order;
    std::size_t           size_then = 0;
    std::binary_semaphore holding{0};

    auto next = noted(&order, 1);

    std::optional<thread_pool> pool{std::in_place, 1};

    // puts next in the slot, then holds the worker until the pool's shutting down.
    auto drive = [](thread_pool*            pool,
                    std::coroutine_handle<> next,
                    std::size_t*            size_then,
                    std::binary_semaphore*  holding) -> net::coro::task<>
    {
        co_await pool->schedule();
        pool->resume(next);
        *size_then = pool->size();
        holding->release();

        // NOTE: schedule() throws once the pool's shutting down, which a test can't otherwise see.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (std::chrono::steady_clock::now() < until)
        {
            try
            {
                (void)pool->schedule();
            }
            catch (const std::runtime_error&)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }(&*pool, handle_of(next), &size_then, &holding);
    drive.get_handle().resume();

    const bool held = holding.try_acquire_for(std::chrono::seconds{10});

    // the destructor shuts it down.
    pool.reset();

    REQUIRE(held);
    // this coroutine and next.
    CHECK(size_then == 2);
    CHECK(order == std::vector{1});
}

namespace
{

// hog resumes next, then runs (awaiting tasks that are ready, so it never returns to its worker) until next has run, or
// for limit; it records how long it waited.
net::coro::task<> hog(thread_pool*                         pool,
                      std::coroutine_handle<>              next,
                      const std::atomic<bool>*             ran,
                      std::chrono::steady_clock::duration  limit,
                      std::chrono::steady_clock::duration* waited)
{
    co_await pool->schedule();
    pool->resume(next);

    const auto start = std::chrono::steady_clock::now();
    while (!ran->load() && std::chrono::steady_clock::now() - start < limit)
    {
        (void)co_await net::coro::ready_task<int>(1);
    }
    *waited = std::chrono::steady_clock::now() - start;
}

net::coro::task<> mark(std::atomic<bool>* ran, std::binary_semaphore* done)
{
    ran->store(true);
    done->release();
    co_return;
}

}

TEST_CASE("a coroutine in a busy worker's slot is run by another, in time", "[coro][thread_pool][slot]")
{
    // NOTE: the bound is about 2 * watchdog_interval (10 ms): generous here, for busy machines.
    constexpr auto bound = std::chrono::milliseconds{500};

    SECTION("without a poller")
    {
        thread_pool pool{2};

        std::atomic<bool>                   ran = false;
        std::binary_semaphore               done{0};
        std::chrono::steady_clock::duration waited{};

        auto next   = mark(&ran, &done);
        auto holder = hog(&pool, handle_of(next), &ran, std::chrono::seconds{10}, &waited);
        holder.get_handle().resume();

        const bool finished = done.try_acquire_for(std::chrono::seconds{10});
        pool.shutdown();

        REQUIRE(finished);
        CHECK(waited < bound);
    }

    SECTION("with a poller, which another worker's blocked in")
    {
        thread_pool pool{2};
        test_poller poller;

        // NOTE: the workers fall asleep before the poller's attached, as a scheduler's usually do. Then one polls,
        //       without a timeout once the other is asleep (the standby), which hog's start wakes: the poll is given
        //       one then, to watch the slots in its place. Timed, as nothing says when the workers are asleep, or
        //       polling: if they aren't yet, the test passes without starting from there.
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        REQUIRE(pool.attach(&poller));
        std::this_thread::sleep_for(std::chrono::milliseconds{50});

        std::atomic<bool>                   ran = false;
        std::binary_semaphore               done{0};
        std::chrono::steady_clock::duration waited{};

        auto next   = mark(&ran, &done);
        auto holder = hog(&pool, handle_of(next), &ran, std::chrono::seconds{10}, &waited);
        holder.get_handle().resume();

        const bool finished = done.try_acquire_for(std::chrono::seconds{10});
        pool.detach(&poller);
        pool.shutdown();

        REQUIRE(finished);
        CHECK(waited < bound);
    }

    SECTION("but not on a pool of one worker, until its coroutine returns (as for what's queued)")
    {
        thread_pool pool{1};

        std::atomic<bool>                   ran = false;
        std::binary_semaphore               done{0};
        std::chrono::steady_clock::duration waited{};

        auto next   = mark(&ran, &done);
        auto holder = hog(&pool, handle_of(next), &ran, std::chrono::milliseconds{100}, &waited);
        holder.get_handle().resume();

        const bool finished = done.try_acquire_for(std::chrono::seconds{10});
        pool.shutdown();

        REQUIRE(finished);
        CHECK(waited >= std::chrono::milliseconds{100});
    }
}
