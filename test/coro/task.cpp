#include "coro/task.hpp"

#include <coroutine>
#include <cstdint>
#include <expected>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

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

namespace
{

// test_operation finishes with value when it's started, or later, when finish() is called.
class test_operation final : public net::coro::operation<int>
{
public:
    bool start(std::coroutine_handle<> awaiting) noexcept override
    {
        started = true;
        if (finish_at_start) return false;

        waiting = awaiting;
        return true;
    }

    int result() override
    {
        release();
        return value;
    }

    void cancel() noexcept override
    {
        cancelled = true;
        release();
    }

    void finish() const { waiting.resume(); }

    bool                    finish_at_start = true;
    bool                    cancelled       = false;
    bool                    started         = false;
    int                     value           = 0;
    std::coroutine_handle<> waiting{nullptr};
};

}

TEST_CASE("operation tasks", "[coro][task][operation]")
{
    test_operation op;

    const auto await_it = [&] -> net::coro::task<int> { co_return co_await net::coro::operation_task(op); };

    SECTION("one that finishes straight away")
    {
        op.value = 5;
        REQUIRE(op.try_claim());

        CHECK(test::sync_wait(await_it()) == 5);

        // taking the result released it.
        CHECK(op.try_claim());
    }

    SECTION("one that finishes later")
    {
        op.finish_at_start = false;
        op.value           = 6;
        REQUIRE(op.try_claim());

        auto awaiting = await_it();
        awaiting.get_handle().resume();
        CHECK_FALSE(awaiting.is_ready());

        op.finish();
        REQUIRE(awaiting.is_ready());
        const int value = std::move(awaiting).result();
        CHECK(value == 6);
    }

    SECTION("destroying the coroutine waiting for one cancels it, rather than leaving it to resume what's gone")
    {
        op.finish_at_start = false;
        REQUIRE(op.try_claim());

        {
            auto awaiting = await_it();
            awaiting.get_handle().resume();
            REQUIRE_FALSE(awaiting.is_ready());
        }

        CHECK(op.cancelled);
        CHECK(op.try_claim());
    }

    SECTION("one that finishes isn't cancelled when the coroutine is destroyed")
    {
        op.value = 7;
        REQUIRE(op.try_claim());

        CHECK(test::sync_wait(await_it()) == 7);
        CHECK_FALSE(op.cancelled);
    }

    SECTION("one that's claimed can't be again until it's released, e.g. as its task is never awaited")
    {
        REQUIRE(op.try_claim());

        {
            auto never_awaited = net::coro::operation_task(op);
            CHECK_FALSE(op.try_claim());
        }

        CHECK_FALSE(op.started);
        CHECK(op.try_claim());
    }
}

TEST_CASE("an empty task can't be awaited, or give a result", "[coro][task]")
{
    net::coro::task<int> empty;
    CHECK_FALSE(empty.valid());
    CHECK_FALSE(empty.resume());
    CHECK_THROWS_AS(std::move(empty).result(), std::logic_error);

    // nor one moved from.
    auto moved_from = [] -> net::coro::task<int> { co_return 1; }();
    auto moved_to   = std::move(moved_from);

    const auto await_it = [](net::coro::task<int>* t) -> net::coro::task<int> { co_return co_await std::move(*t); };
    // NOLINTNEXTLINE(bugprone-use-after-move): what's left of it is what's checked.
    CHECK_THROWS_AS(test::sync_wait(await_it(&moved_from)), std::logic_error);
    const int value = test::sync_wait(std::move(moved_to));
    CHECK(value == 1);
}
