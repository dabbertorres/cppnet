#include "coro/adapter.hpp"

#include <array>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

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

// summer is a stepper's owner: its steps add up the inner tasks' results, each step's inner task made by next (given
// the step's number, from 0) until there have been steps of them; a negative result throws. released records, each
// time a task of the stepper's has given its result, whether the stepper was free by then.
struct summer
{
    net::coro::step<int, int> add(int value)
    {
        if (value < 0) throw std::runtime_error{"negative"};

        sum += value;
        if (++taken == steps) return sum;
        return next(taken);
    }

    void released() noexcept { releases.push_back(!stepping.is_claimed()); }

    // start makes a task of the stepper, for steps inner tasks.
    task<int> start(int count)
    {
        steps = count;
        taken = 0;
        sum   = 0;
        return stepping.adapt([this] { return next(0); }, busy);
    }

    std::function<task<int>(int)> next;
    int                           steps = 0;
    int                           taken = 0;
    int                           sum   = 0;
    std::vector<bool>             releases;

    net::coro::stepper<summer, int, int, &summer::add, &summer::released> stepping{this};
};

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

TEST_CASE("an adapter's reset() lets go of what a task dropped unawaited left, but not of a task under way",
          "[coro][adapter][busy]")
{
    doubler        d;
    test_operation op;
    op.finish_at_start = false;
    op.value           = 3;
    REQUIRE(op.try_claim());

    SECTION("left by a task dropped unawaited")
    {
        {
            auto never_awaited = d.adapting.adapt([&] { return net::coro::operation_task(op); }, busy);
        }
        CHECK_FALSE(op.try_claim());

        d.adapting.reset();
        CHECK_FALSE(op.started);
        CHECK(op.try_claim());
    }

    SECTION("under way")
    {
        auto awaiting = await_it(d.adapting.adapt([&] { return net::coro::operation_task(op); }, busy));
        awaiting.get_handle().resume();
        REQUIRE_FALSE(awaiting.is_ready());

        d.adapting.reset();
        CHECK(d.adapting.is_claimed());

        op.finish();
        REQUIRE(awaiting.is_ready());
        const int value = std::move(awaiting).result();
        CHECK(value == 6);
        CHECK(op.try_claim());
    }
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

TEST_CASE("a stepper takes the steps of inner tasks that finish at once without itself", "[coro][stepper]")
{
    summer s;
    s.next = [](int step) { return net::coro::ready_task<int>(step + 1); };

    auto t = s.start(3);
    REQUIRE(t.finished_without_coroutine());
    CHECK_FALSE(s.stepping.is_claimed());

    const int value = std::move(t).result();
    CHECK(value == 6);
    CHECK(s.releases.empty());
}

TEST_CASE(
    "a stepper's inner tasks continue its driver, which resumes the awaiting coroutine once the last step's taken",
    "[coro][stepper][operation]")
{
    summer                        s;
    std::array<test_operation, 3> ops;
    for (std::size_t i = 0; i < ops.size(); ++i) ops.at(i).value = static_cast<int>(i) + 1;

    s.next = [&](int step)
    {
        auto& op = ops.at(static_cast<std::size_t>(step));
        REQUIRE(op.try_claim());
        return net::coro::operation_task(op);
    };

    SECTION("each waits")
    {
        for (auto& op : ops) op.finish_at_start = false;

        auto awaiting = await_it(s.start(3));
        awaiting.get_handle().resume();
        REQUIRE_FALSE(awaiting.is_ready());
        CHECK(s.stepping.is_claimed());

        // the first was started with the driver to continue, not the awaiting coroutine.
        REQUIRE(ops[0].started);
        CHECK(ops[0].waiting.address() != awaiting.get_handle().address());

        // its step starts the next, for the same driver; the awaiting coroutine waits on.
        ops[0].finish();
        REQUIRE(ops[1].started);
        CHECK(ops[1].waiting.address() == ops[0].waiting.address());
        CHECK_FALSE(awaiting.is_ready());

        ops[1].finish();
        REQUIRE(ops[2].started);
        CHECK_FALSE(awaiting.is_ready());

        ops[2].finish();
        REQUIRE(awaiting.is_ready());
        const int value = std::move(awaiting).result();
        CHECK(value == 6);
    }

    SECTION("only the middle one waits: the others' steps are taken where they finish")
    {
        ops[1].finish_at_start = false;

        auto awaiting = await_it(s.start(3));
        awaiting.get_handle().resume();
        REQUIRE_FALSE(awaiting.is_ready());
        CHECK(ops[0].started);
        CHECK(ops[1].started);
        CHECK_FALSE(ops[2].started);

        ops[1].finish();
        REQUIRE(awaiting.is_ready());
        const int value = std::move(awaiting).result();
        CHECK(value == 6);
    }

    SECTION("all finish when they're started: the awaiting coroutine doesn't suspend")
    {
        CHECK(test::sync_wait(s.start(3)) == 6);
    }

    // the stepper's free, and was by the time it said its task had given its result; each operation's done with.
    CHECK_FALSE(s.stepping.is_claimed());
    CHECK(s.releases == std::vector<bool>{true});
    for (auto& op : ops)
    {
        CHECK(op.try_claim());
        CHECK_FALSE(op.cancelled);
    }
}

TEST_CASE("a stepper keeps its driver for its next task", "[coro][stepper][operation]")
{
    summer         s;
    test_operation op;
    op.finish_at_start = false;
    op.value           = 2;
    s.next             = [&](int /*step*/)
    {
        REQUIRE(op.try_claim());
        return net::coro::operation_task(op);
    };

    std::vector<void*> drivers;
    for (int round = 0; round < 2; ++round)
    {
        auto awaiting = await_it(s.start(2));
        awaiting.get_handle().resume();
        REQUIRE_FALSE(awaiting.is_ready());
        drivers.push_back(op.waiting.address());

        op.finish();
        REQUIRE_FALSE(awaiting.is_ready());
        op.finish();
        REQUIRE(awaiting.is_ready());
        const int value = std::move(awaiting).result();
        CHECK(value == 4);
    }

    CHECK(drivers[0] == drivers[1]);
    CHECK(s.releases == std::vector<bool>{true, true});
}

TEST_CASE("a stepper's inner coroutines continue its driver too", "[coro][stepper][task]")
{
    summer         s;
    test_operation op;
    op.finish_at_start = false;
    op.value           = 4;
    REQUIRE(op.try_claim());

    // a coroutine that suspends, then one that finishes when it's started (5 + 1).
    s.next = [&](int step)
    {
        if (step == 1) return net::coro::ready_task<int>(1);
        return plus_one(&op);
    };

    auto awaiting = await_it(s.start(2));
    awaiting.get_handle().resume();
    REQUIRE_FALSE(awaiting.is_ready());

    op.finish();
    REQUIRE(awaiting.is_ready());
    const int value = std::move(awaiting).result();
    CHECK(value == 6);
    CHECK_FALSE(s.stepping.is_claimed());
    CHECK(op.try_claim());
}

TEST_CASE("a stepper rethrows what an inner coroutine or a step threw", "[coro][stepper][task]")
{
    summer         s;
    test_operation op;
    op.finish_at_start = false;
    REQUIRE(op.try_claim());

    SECTION("an inner coroutine, after a wait: no more steps are taken")
    {
        s.next = [&](int step)
        {
            if (step == 0) return throws_after(&op);
            return net::coro::ready_task<int>(1);
        };

        auto awaiting = await_it(s.start(2));
        awaiting.get_handle().resume();
        REQUIRE_FALSE(awaiting.is_ready());

        op.finish();
        REQUIRE(awaiting.is_ready());
        CHECK_THROWS_AS(std::move(awaiting).result(), std::runtime_error);
        CHECK(s.taken == 0);
    }

    SECTION("a step, after a wait")
    {
        op.value = -1;
        s.next   = [&](int /*step*/) { return net::coro::operation_task(op); };

        auto awaiting = await_it(s.start(2));
        awaiting.get_handle().resume();
        REQUIRE_FALSE(awaiting.is_ready());

        op.finish();
        REQUIRE(awaiting.is_ready());
        CHECK_THROWS_AS(std::move(awaiting).result(), std::runtime_error);
    }

    CHECK_FALSE(s.stepping.is_claimed());
    CHECK(s.releases == std::vector<bool>{true});
    CHECK(op.try_claim());
}

TEST_CASE("a stepper's step that throws without it throws from adapt()", "[coro][stepper]")
{
    summer s;
    s.next = [](int /*step*/) { return net::coro::ready_task<int>(-1); };

    CHECK_THROWS_AS(s.start(2), std::runtime_error);
    CHECK_FALSE(s.stepping.is_claimed());
}

TEST_CASE("a stepper is for one task at a time", "[coro][stepper][busy]")
{
    summer         s;
    test_operation op;
    op.finish_at_start = false;
    op.value           = 3;
    REQUIRE(op.try_claim());
    s.next = [&](int /*step*/) { return net::coro::operation_task(op); };

    auto awaiting = await_it(s.start(1));
    awaiting.get_handle().resume();
    REQUIRE_FALSE(awaiting.is_ready());

    // another while it waits is busy, without making its first inner task.
    bool made  = false;
    auto other = s.stepping.adapt(
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

    op.finish();
    REQUIRE(awaiting.is_ready());
    const int value = std::move(awaiting).result();
    CHECK(value == 3);
    CHECK_FALSE(s.stepping.is_claimed());
}

TEST_CASE("a stepper's task dropped unawaited frees the stepper, and leaves its inner task to the next adapt()",
          "[coro][stepper][busy]")
{
    summer         s;
    test_operation op;
    REQUIRE(op.try_claim());
    s.next = [&](int /*step*/) { return net::coro::operation_task(op); };

    {
        auto never_awaited = s.start(1);
        CHECK(s.stepping.is_claimed());
    }
    CHECK_FALSE(s.stepping.is_claimed());

    // the inner task, still the stepper's, holds the operation, unstarted, until the next adapt() drops it.
    CHECK_FALSE(op.try_claim());
    s.next          = [](int /*step*/) { return net::coro::ready_task<int>(1); };
    auto      next  = s.start(1);
    const int value = std::move(next).result();
    CHECK(value == 1);
    CHECK_FALSE(op.started);
    CHECK(op.try_claim());
    CHECK(s.releases.empty());
}

TEST_CASE("destroying the coroutine awaiting a stepper cancels the inner task waiting", "[coro][stepper][cancel]")
{
    summer                        s;
    std::array<test_operation, 2> ops;
    for (auto& op : ops) op.finish_at_start = false;
    test_operation inner_op;
    inner_op.finish_at_start = false;
    REQUIRE(inner_op.try_claim());

    SECTION("an inner operation, after a step")
    {
        s.next = [&](int step)
        {
            auto& op = ops.at(static_cast<std::size_t>(step));
            REQUIRE(op.try_claim());
            return net::coro::operation_task(op);
        };

        {
            auto awaiting = await_it(s.start(2));
            awaiting.get_handle().resume();
            ops[0].finish();
            REQUIRE(ops[1].started);
            REQUIRE_FALSE(awaiting.is_ready());
        }

        CHECK_FALSE(ops[0].cancelled);
        CHECK(ops[1].cancelled);
    }

    SECTION("an inner coroutine is destroyed, which cancels what it waits for")
    {
        s.next = [&](int /*step*/) { return plus_one(&inner_op); };

        {
            auto awaiting = await_it(s.start(1));
            awaiting.get_handle().resume();
            REQUIRE_FALSE(awaiting.is_ready());
        }

        CHECK(inner_op.cancelled);
    }

    CHECK_FALSE(s.stepping.is_claimed());
    CHECK(s.releases.empty());

    // and it's ready for the next.
    s.next = [](int /*step*/) { return net::coro::ready_task<int>(5); };
    CHECK(test::sync_wait(s.start(1)) == 5);
}
