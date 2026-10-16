#include "coro/task.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"

TEST_CASE("task of std::expected can be destroyed", "[coro][task]")
{
    // Comparing the task's handle to nullptr used to find std::expected's operator== through ADL, which some standard
    // libraries don't constrain, and so recursed forever.
    auto make = [] -> net::coro::task<std::expected<int*, std::error_condition>> { co_return nullptr; };

    auto value = test::sync_wait(make());
    REQUIRE(value.has_value());
    CHECK(*value == nullptr);
}

namespace
{

net::coro::task<int> one() { co_return 1; }

net::coro::task<std::int64_t> count_to(std::int64_t n)
{
    std::int64_t total = 0;
    for (std::int64_t i = 0; i < n; ++i) total += co_await one();
    co_return total;
}

}

TEST_CASE("co_awaiting tasks that finish without suspending doesn't grow the stack", "[coro][task]")
{
    // With symmetric transfer, each of these nests a little deeper, unless the compiler makes the transfers tail calls
    // (which GCC doesn't without optimizations, or with ASan), and the stack overflows.
    CHECK(test::sync_wait(count_to(1'000'000)) == 1'000'000);
}

TEST_CASE("ready tasks have their result without a coroutine", "[coro][task][ready]")
{
    using net::coro::ready_task;
    using net::coro::task;

    SECTION("a value, awaited as an rvalue or an lvalue")
    {
        auto run = [] -> task<std::string>
        {
            auto first = co_await ready_task<std::string>("first");

            auto lvalue = ready_task<std::string>("second");
            auto second = co_await lvalue;
            auto again  = co_await lvalue; // an lvalue's result is copied, so it's still there.

            co_return first + second + again;
        };

        CHECK(test::sync_wait(run()) == "firstsecondsecond");
    }

    SECTION("void")
    {
        auto run = [] -> task<int>
        {
            co_await ready_task();
            co_return 1;
        };

        CHECK(test::sync_wait(run()) == 1);
    }

    SECTION("a reference")
    {
        int  value = 1;
        auto run   = [&] -> task<int>
        {
            int& ref = co_await ready_task<int&>(value);
            ref      = 2;
            co_return value;
        };

        CHECK(test::sync_wait(run()) == 2);
    }

    SECTION("move-only")
    {
        auto run = [] -> task<int> { co_return *co_await ready_task<std::unique_ptr<int>>(std::make_unique<int>(3)); };

        CHECK(test::sync_wait(run()) == 3);
    }

    SECTION("without awaiting it")
    {
        auto ready = ready_task<int>(4);
        CHECK(ready.valid());
        CHECK(ready.is_ready());
        CHECK(ready.finished_without_coroutine());
        const int value = std::move(ready).result();
        CHECK(value == 4);

        // a coroutine's task isn't.
        CHECK_FALSE(one().finished_without_coroutine());
    }
}
