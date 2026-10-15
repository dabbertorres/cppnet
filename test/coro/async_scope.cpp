#include "coro/async_scope.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"

namespace
{

using net::coro::async_scope;
using net::coro::task;

// manual_event suspends the awaiting coroutine until it's resumed by hand.
struct manual_event
{
    std::coroutine_handle<> waiting{nullptr};

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void               await_suspend(std::coroutine_handle<> handle) noexcept { waiting = handle; }
    void               await_resume() const noexcept {}

    void set() { std::exchange(waiting, nullptr).resume(); }
};

task<> hold(std::shared_ptr<int> /*held*/, int* ran)
{
    ++*ran;
    co_return;
}

task<> wait_for(manual_event* event, bool* done)
{
    co_await *event;
    *done = true;
}

task<> throw_runtime_error()
{
    throw std::runtime_error{"boom"};
    co_return;
}

task<> join(async_scope* scope, bool* joined)
{
    co_await scope->join();
    *joined = true;
}

}

TEST_CASE("spawned tasks run, and are destroyed when they finish", "[coro][async_scope]")
{
    async_scope scope;

    auto held = std::make_shared<int>();
    int  ran  = 0;

    REQUIRE(scope.spawn(hold(held, &ran)));

    CHECK(ran == 1);
    CHECK(scope.empty());
    CHECK(held.use_count() == 1);
}

TEST_CASE("a suspended task stays in the scope until it finishes", "[coro][async_scope]")
{
    async_scope  scope;
    manual_event event;
    bool         done = false;

    REQUIRE(scope.spawn(wait_for(&event, &done)));
    CHECK(scope.size() == 1);
    CHECK_FALSE(done);

    event.set();

    CHECK(done);
    CHECK(scope.empty());
}

TEST_CASE("a closed scope destroys new tasks without running them", "[coro][async_scope]")
{
    async_scope scope;
    scope.close();

    auto held = std::make_shared<int>();
    int  ran  = 0;

    CHECK(scope.closed());
    CHECK_FALSE(scope.spawn(hold(held, &ran)));
    CHECK(ran == 0);
    CHECK(held.use_count() == 1);
}

TEST_CASE("exceptions escaping a task are passed to the handler", "[coro][async_scope]")
{
    std::string message;

    async_scope scope{[&](const std::exception_ptr& ex)
                      {
                          try
                          {
                              std::rethrow_exception(ex);
                          }
                          catch (const std::runtime_error& err)
                          {
                              message = err.what();
                          }
                      }};

    REQUIRE(scope.spawn(throw_runtime_error()));
    CHECK(message == "boom");
    CHECK(scope.empty());
}

TEST_CASE("join completes once the last task finishes", "[coro][async_scope]")
{
    async_scope scope;

    SECTION("immediately, when empty")
    {
        bool joined = false;
        auto joiner = join(&scope, &joined);
        joiner.get_handle().resume();

        CHECK(joined);
    }

    SECTION("after the last task, otherwise")
    {
        manual_event event;
        bool         done = false;
        REQUIRE(scope.spawn(wait_for(&event, &done)));

        bool joined = false;
        auto joiner = join(&scope, &joined);
        joiner.get_handle().resume();
        CHECK_FALSE(joined);

        event.set();
        CHECK(joined);
    }
}

TEST_CASE("the drained handler is called once the scope is closed and empty", "[coro][async_scope]")
{
    int drained = 0;

    async_scope scope{{}, [&] { ++drained; }};

    manual_event event;
    bool         done = false;
    REQUIRE(scope.spawn(wait_for(&event, &done)));

    scope.close();
    CHECK(drained == 0);

    event.set();
    CHECK(drained == 1);

    scope.close();
    CHECK(drained == 1);
}

TEST_CASE("the destructor waits for tasks running on other threads", "[coro][async_scope]")
{
    using namespace std::chrono_literals;

    constexpr int num_spawners       = 4;
    constexpr int tasks_per_spawner  = 250;
    constexpr int expected_completed = num_spawners * tasks_per_spawner;

    net::coro::thread_pool pool{4};
    std::atomic<int>       completed = 0;

    {
        async_scope scope;

        auto work = [](net::coro::thread_pool* pool, std::atomic<int>* completed, int index) -> task<>
        {
            co_await pool->schedule();
            if (index % 50 == 0) std::this_thread::sleep_for(1ms);
            completed->fetch_add(1, std::memory_order::relaxed);
        };

        std::vector<std::thread> spawners;
        spawners.reserve(num_spawners);
        for (int i = 0; i < num_spawners; ++i)
        {
            spawners.emplace_back(
                [&]
                {
                    for (int j = 0; j < tasks_per_spawner; ++j) scope.spawn(work(&pool, &completed, j));
                });
        }

        for (auto& spawner : spawners) spawner.join();
    }

    CHECK(completed.load() == expected_completed);
}
