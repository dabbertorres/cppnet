#pragma once

#include <coroutine>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include "coro/task.hpp"
#include "util/defer.hpp"

namespace net::coro
{

// adapter is an operation for a function that makes a task of another task (its inner one, e.g. a read of what it
// wraps), finishing with a step of its own on that one's result (Finish, a member function of Owner, e.g. counting what
// was read): what a coroutine awaiting the inner task would do, without a coroutine, so without a frame. An object
// holds one (as a socket holds its read operation), and makes its tasks with adapt().
//
// Awaited, it starts the inner task in place: an operation (e.g. a socket's read that has to wait) is started for the
// awaiting coroutine, which the operation then resumes itself, with nothing between; a coroutine's task is resumed with
// the awaiting coroutine to continue once it's finished, as awaiting it would. Its result is what Finish makes of the
// inner one's (rethrowing what that threw).
//
// As any operation, it's for one task at a time: adapt() makes a ready task of busy while one of it is under way.
// Destroying the coroutine awaiting it while it waits cancels the inner task too (an operation's cancel(), or a
// coroutine's frame destroyed, which cancels what that waits for in turn). A task of it that's destroyed without being
// awaited leaves its inner task with the adapter until the next adapt() (or the adapter's end), which drops it.
template<typename Owner, typename In, typename Out, auto Finish>
class adapter final : public operation<Out>
{
public:
    explicit adapter(Owner* owner) noexcept
        : owner{owner}
    {}

    adapter(const adapter&)            = delete;
    adapter(adapter&&)                 = delete;
    adapter& operator=(const adapter&) = delete;
    adapter& operator=(adapter&&)      = delete;
    // (an inner task left by a task of it dropped unawaited goes with it: what that holds, e.g. a socket's read, must
    // still be there.)
    ~adapter() { drop(); }

    // adapt makes a task of what make() returns (a task<In>, made only if the adapter is free), which finishes with
    // Finish: at once, without the adapter, if that one's finished already (a ready task, e.g. a read of what's
    // buffered); otherwise a task of the adapter. A ready task of busy if a task of the adapter is under way.
    template<typename Make>
        requires(std::is_invocable_r_v<task<In>, Make>)
    task<Out> adapt(Make&& make, Out busy)
    {
        if (this->is_claimed()) return ready_task<Out>(std::move(busy));

        // (one left by a task of it that was dropped unawaited may hold what make() needs, e.g. a socket's read.)
        drop();

        auto made = std::invoke(std::forward<Make>(make));
        if (made.finished_without_coroutine()) return ready_task<Out>(finish(std::move(made).result()));
        if (!this->try_claim()) return ready_task<Out>(std::move(busy));

        // the inner task's coroutine or operation is the adapter's now (only those, not the task: a smaller adapter).
        handle = std::exchange(made.handle, nullptr);
        op     = std::exchange(made.op, nullptr);
        return operation_task<Out>(*this);
    }

    // NOLINTBEGIN(portability-template-virtual-member-function): overriders, which every compiler instantiates with
    // the class (they're in its vtable).
    bool start(std::coroutine_handle<> awaiting) noexcept override
    {
        // NOTE: once the inner task's started, it may finish and resume awaiting (on another thread) at any moment, so
        //       nothing here is touched after.
        if (op != nullptr) return op->start(awaiting);

        typename task<In>::awaitable_base handshake{handle, std::nullopt, nullptr};
        return handshake.await_suspend(awaiting);
    }

    Out result() override
    {
        // NOTE: after Finish, which the inner coroutine's result (a reference into its frame) is passed to.
        const util::defer done{[this] noexcept { finished(); }};
        if (op != nullptr) return finish(op->result());
        return finish(std::move(handle.promise()).result());
    }

    void cancel() noexcept override
    {
        if (op != nullptr) op->cancel();
        finished();
    }
    // NOLINTEND(portability-template-virtual-member-function)

private:
    Out finish(In&& value)
        requires(std::is_invocable_r_v<Out, decltype(Finish), Owner&, In&&>)
    {
        return std::invoke(Finish, *owner, std::move(value));
    }

    // drop lets go of an inner task that was never started (left by a task of the adapter dropped unawaited).
    void drop() noexcept
    {
        if (handle) std::exchange(handle, nullptr).destroy();
        if (op != nullptr) std::exchange(op, nullptr)->release();
    }

    // finished lets go of the inner task, which was started (its operation's done with, by result() or cancel(); its
    // coroutine's frame is destroyed: one still waiting cancels what it waits for), and the adapter.
    void finished() noexcept
    {
        if (handle) std::exchange(handle, nullptr).destroy();
        op = nullptr;
        this->release();
    }

    Owner*                                                 owner;
    std::coroutine_handle<typename task<In>::promise_type> handle{nullptr};
    operation<In>*                                         op = nullptr;
};

}
