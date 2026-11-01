#include "coro/adapter.hpp"

#include <coroutine>
#include <stdexcept>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"

namespace
{

using net::coro::task;

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

// doubler is an adapter's owner: its finishing step doubles the inner task's result, counting the times it's run.
struct doubler
{
    int twice(int value) noexcept
    {
        ++finished;
        return value * 2;
    }

    int                                                    finished = 0;
    net::coro::adapter<doubler, int, int, &doubler::twice> adapting{this};
};

constexpr int busy = -1;

// await_it awaits t, as a coroutine using an adapter's task does.
task<int> await_it(task<int> t) { co_return co_await std::move(t); }

// plus_one awaits op (which must be claimed), adding one to its result: an inner coroutine.
task<int> plus_one(test_operation* op) { co_return co_await net::coro::operation_task(*op) + 1; }

// throws_after awaits op (which must be claimed), then throws.
task<int> throws_after(test_operation* op)
{
    (void)co_await net::coro::operation_task(*op);
    throw std::runtime_error{"inner failure"};
}

}

TEST_CASE("an adapter finishes a ready inner task at once, without itself", "[coro][adapter]")
{
    doubler d;

    auto t = d.adapting.adapt([] { return net::coro::ready_task<int>(3); }, busy);
    REQUIRE(t.finished_without_coroutine());
    CHECK_FALSE(d.adapting.is_claimed());

    const int value = std::move(t).result();
    CHECK(value == 6);
    CHECK(d.finished == 1);
}

TEST_CASE("an adapter starts an inner operation for the coroutine awaiting it", "[coro][adapter][operation]")
{
    doubler        d;
    test_operation op;

    SECTION("one that finishes straight away")
    {
        op.value = 5;
        REQUIRE(op.try_claim());

        auto t = d.adapting.adapt([&] { return net::coro::operation_task(op); }, busy);
        REQUIRE_FALSE(t.finished_without_coroutine());
        CHECK(d.adapting.is_claimed());

        const int value = test::sync_wait(await_it(std::move(t)));
        CHECK(value == 10);
        CHECK(d.finished == 1);
        CHECK(op.started);
    }

    SECTION("one that finishes later resumes the awaiting coroutine itself")
    {
        op.finish_at_start = false;
        op.value           = 6;
        REQUIRE(op.try_claim());

        auto awaiting = await_it(d.adapting.adapt([&] { return net::coro::operation_task(op); }, busy));
        awaiting.get_handle().resume();
        REQUIRE_FALSE(awaiting.is_ready());

        // the operation was given the awaiting coroutine: nothing runs between them.
        CHECK(op.waiting.address() == awaiting.get_handle().address());
        CHECK(d.finished == 0);

        op.finish();
        REQUIRE(awaiting.is_ready());
        const int value = std::move(awaiting).result();
        CHECK(value == 12);
        CHECK(d.finished == 1);
    }

    // the adapter and the inner operation were both released.
    CHECK_FALSE(d.adapting.is_claimed());
    CHECK(op.try_claim());
    CHECK_FALSE(op.cancelled);
}

TEST_CASE("an adapter resumes an inner coroutine for the coroutine awaiting it", "[coro][adapter][task]")
{
    doubler        d;
    test_operation op;
    REQUIRE(op.try_claim());

    SECTION("one that finishes without suspending")
    {
        op.value = 4;

        CHECK(test::sync_wait(await_it(d.adapting.adapt([&] { return plus_one(&op); }, busy))) == 10);
    }

    SECTION("one that suspends, and resumes the awaiting coroutine when it finishes")
    {
        op.finish_at_start = false;
        op.value           = 7;

        auto awaiting = await_it(d.adapting.adapt([&] { return plus_one(&op); }, busy));
        awaiting.get_handle().resume();
        REQUIRE_FALSE(awaiting.is_ready());
        CHECK(d.finished == 0);

        op.finish();
        REQUIRE(awaiting.is_ready());
        const int value = std::move(awaiting).result();
        CHECK(value == 16);
    }

    CHECK(d.finished == 1);
    CHECK_FALSE(d.adapting.is_claimed());
    CHECK(op.try_claim());
}

TEST_CASE("an adapter rethrows what its inner coroutine threw, skipping its finishing step", "[coro][adapter][task]")
{
    doubler        d;
    test_operation op;
    op.finish_at_start = false;
    REQUIRE(op.try_claim());

    auto awaiting = await_it(d.adapting.adapt([&] { return throws_after(&op); }, busy));
    awaiting.get_handle().resume();
    REQUIRE_FALSE(awaiting.is_ready());

    op.finish();
    REQUIRE(awaiting.is_ready());
    CHECK_THROWS_AS(std::move(awaiting).result(), std::runtime_error);
    CHECK(d.finished == 0);
    CHECK_FALSE(d.adapting.is_claimed());
}

TEST_CASE("an adapter is for one task at a time", "[coro][adapter][busy]")
{
    doubler        d;
    test_operation op;
    op.finish_at_start = false;
    op.value           = 2;
    REQUIRE(op.try_claim());

    auto awaiting = await_it(d.adapting.adapt([&] { return net::coro::operation_task(op); }, busy));
    awaiting.get_handle().resume();
    REQUIRE_FALSE(awaiting.is_ready());

    // another while it waits is busy, without making its inner task.
    bool made  = false;
    auto other = d.adapting.adapt(
        [&]
        {
            made = true;
            return net::coro::ready_task<int>(1);
        },
        busy);
    REQUIRE(other.finished_without_coroutine());
    const int other_value = std::move(other).result();
    CHECK(other_value == busy);
    CHECK_FALSE(made);

    // and once it's finished, the adapter's free again.
    op.finish();
    REQUIRE(awaiting.is_ready());
    const int value = std::move(awaiting).result();
    CHECK(value == 4);

    REQUIRE(op.try_claim());
    op.finish_at_start = true;
    CHECK(test::sync_wait(await_it(d.adapting.adapt([&] { return net::coro::operation_task(op); }, busy))) == 4);
}

TEST_CASE("an adapter's task dropped unawaited frees the adapter, and leaves its inner task to the next adapt()",
          "[coro][adapter][busy]")
{
    doubler        d;
    test_operation op;
    REQUIRE(op.try_claim());

    {
        auto never_awaited = d.adapting.adapt([&] { return net::coro::operation_task(op); }, busy);
        CHECK(d.adapting.is_claimed());
    }
    CHECK_FALSE(d.adapting.is_claimed());

    // the inner task, still the adapter's, holds the operation, unstarted, until the next adapt() drops it.
    CHECK_FALSE(op.try_claim());
    auto      next  = d.adapting.adapt([&] { return net::coro::ready_task<int>(1); }, busy);
    const int value = std::move(next).result();
    CHECK(value == 2);
    CHECK_FALSE(op.started);
    CHECK(op.try_claim());
}

TEST_CASE("destroying the coroutine awaiting an adapter cancels its inner task", "[coro][adapter][cancel]")
{
    doubler        d;
    test_operation op;
    op.finish_at_start = false;
    REQUIRE(op.try_claim());

    SECTION("an inner operation is canceled")
    {
        auto awaiting = await_it(d.adapting.adapt([&] { return net::coro::operation_task(op); }, busy));
        awaiting.get_handle().resume();
        REQUIRE_FALSE(awaiting.is_ready());
    }

    SECTION("an inner coroutine is destroyed, which cancels what it waits for")
    {
        auto awaiting = await_it(d.adapting.adapt([&] { return plus_one(&op); }, busy));
        awaiting.get_handle().resume();
        REQUIRE_FALSE(awaiting.is_ready());
    }

    CHECK(op.cancelled);
    CHECK(d.finished == 0);
    CHECK_FALSE(d.adapting.is_claimed());
    CHECK(op.try_claim());
}
