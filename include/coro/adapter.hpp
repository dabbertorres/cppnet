#pragma once

#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "coro/task.hpp"
#include "util/defer.hpp"

namespace net::coro
{

namespace detail
{

    // busy_value is an adapter's (or a stepper's) result when it's busy: busy itself, or what it makes, if it's a
    // function (so that a result that costs something to make, e.g. an error code, is only made when it's needed).
    template<typename Out, typename Busy>
    Out busy_value(Busy&& busy)
    {
        if constexpr (std::is_invocable_r_v<Out, Busy>) return std::invoke(std::forward<Busy>(busy));
        else return Out(std::forward<Busy>(busy));
    }

}

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
    // (an inner task left by a task of it dropped unawaited goes with it: what that holds, e.g. a socket's read, must
    // still be there.)
    ~adapter() { drop(); }

    // An owner that's assigned (e.g. made anew in place, to be reused for another body) keeps its own adapter, which
    // must have no task under way: what it holds (an inner task left by a task of it dropped unawaited) is dropped.
    adapter& operator=(adapter&& /*other*/) noexcept
    {
        drop();
        return *this;
    }

    // adapt makes a task of what make() returns (a task<In>, made only if the adapter is free), which finishes with
    // Finish: at once, without the adapter, if that one's finished already (a ready task, e.g. a read of what's
    // buffered); otherwise a task of the adapter. A ready task of busy (or what it makes, if it's a function) if a task
    // of the adapter is under way.
    template<typename Make, typename Busy>
        requires(std::is_invocable_r_v<task<In>, Make>
                 && (std::is_invocable_r_v<Out, Busy> || std::is_constructible_v<Out, Busy>))
    task<Out> adapt(Make&& make, Busy&& busy)
    {
        if (this->is_claimed()) return ready_task<Out>(detail::busy_value<Out>(std::forward<Busy>(busy)));

        // (one left by a task of it that was dropped unawaited may hold what make() needs, e.g. a socket's read.)
        drop();

        auto made = std::invoke(std::forward<Make>(make));
        if (made.finished_without_coroutine()) return ready_task<Out>(finish(std::move(made).result()));
        if (!this->try_claim()) return ready_task<Out>(detail::busy_value<Out>(std::forward<Busy>(busy)));

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

// step is what a stepper's step makes of an inner task's result: the stepper's own result (Out), or the next inner task
// to await.
template<typename In, typename Out>
using step = std::variant<Out, task<In>>;

namespace detail
{

    // step_driver is the coroutine a stepper's inner tasks continue, rather than the coroutine awaiting the stepper, so
    // that the stepper takes its steps between them (see stepper). A stepper makes it the first time one of its inner
    // tasks has to wait, and keeps it, parked between its tasks: so its tasks that wait don't each make a coroutine.
    class step_driver
    {
    public:
        // advance takes a stepper's steps once its inner task has finished, returning the coroutine to resume: the one
        // awaiting the stepper, once its result's in, or none (a no-op), if another inner task waits (which resumes the
        // driver in turn).
        using advance_function = std::coroutine_handle<> (*)(void* stepper) noexcept;

        struct promise_type
        {
            step_driver get_return_object() noexcept
            {
                return step_driver{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // it's started by the first inner task to finish, and never finishes.
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend() noexcept { return {}; }
            void                return_void() noexcept {}
            void                unhandled_exception() noexcept { std::terminate(); }
        };

        step_driver() noexcept = default;

        step_driver(const step_driver&)            = delete;
        step_driver& operator=(const step_driver&) = delete;

        step_driver(step_driver&& other) noexcept
            : handle{std::exchange(other.handle, nullptr)}
        {}

        step_driver& operator=(step_driver&& other) noexcept
        {
            if (this != &other)
            {
                if (handle) handle.destroy();
                handle = std::exchange(other.handle, nullptr);
            }
            return *this;
        }

        ~step_driver()
        {
            if (handle) handle.destroy();
        }

        // drive makes a stepper's driver: each time it's resumed, it advances the stepper.
        static step_driver drive(advance_function advance, void* stepper)
        {
            // NOTE: nothing's touched here once the stepper's been advanced: an inner task it started may resume the
            //       driver (on another thread) at any moment, which only ever comes back in through await_resume.
            while (true) co_await advanced{.advance = advance, .stepper = stepper};
        }

        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handle); }

        [[nodiscard]] std::coroutine_handle<> get() const noexcept { return handle; }

    private:
        explicit step_driver(std::coroutine_handle<promise_type> handle) noexcept
            : handle{handle}
        {}

        // advanced is the driver's wait for an inner task, which resumes it once it's finished: then (the driver
        // suspended again, as an inner task it starts may resume it at once) it advances the stepper, and goes on to
        // what that says.
        struct advanced
        {
            advance_function advance;
            void*            stepper;

            [[nodiscard]] bool                    await_ready() const noexcept { return false; }
            [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> /*driver*/) const noexcept
            {
                return advance(stepper);
            }
            void await_resume() const noexcept { /* noop */ }
        };

        std::coroutine_handle<promise_type> handle{nullptr};
    };

}

// stepper is an adapter whose function awaits inner tasks in turn (e.g. writes, until all's written): its step (Step, a
// member function of Owner) makes of each inner task's result its own result, or the next inner task. What a coroutine
// awaiting them in a loop would do, without a coroutine of its own for each task of it.
//
// adapt(make, busy) takes the first step (make()), and the steps after it, while their inner tasks finish without
// waiting (ready tasks), without the stepper: then its task is a ready one. Otherwise its task is the stepper's
// (claimed for it), which starts the inner task when it's awaited, with the stepper's driver (see detail::step_driver)
// to continue: so the stepper takes the next steps itself, as each inner task finishes, until its result's in, and then
// resumes the awaiting coroutine. That's one coroutine (the driver) per stepper, made the first time one of its inner
// tasks waits, and kept; and one more resume per wait, the driver's.
//
// Its result is the last step's (rethrowing what a step threw, or an inner coroutine threw). As an adapter, it's for
// one task at a time (busy otherwise, without make() called); destroying the coroutine awaiting it while it waits
// cancels the inner task waiting; a task of it dropped unawaited leaves its inner task to the next adapt() (or the
// stepper's end).
//
// Released, if given (a member function of Owner), is called once a task of the stepper's has given its result, after
// the stepper's been let go: for an owner whose steps let go of something that the stepper's next task depends on (e.g.
// a connection's turn to write, the next holder of which uses the stepper), which a step can't do while the stepper's
// still the last one's. (Steps that run without the stepper, in adapt(), can.)
template<typename Owner, typename In, typename Out, auto Step, auto Released = nullptr>
class stepper final : public operation<Out>
{
public:
    explicit stepper(Owner* owner) noexcept
        : owner{owner}
    {}

    stepper(const stepper&)            = delete;
    stepper(stepper&&)                 = delete;
    stepper& operator=(const stepper&) = delete;
    stepper& operator=(stepper&&)      = delete;
    ~stepper() { drop(); }

    // adapt makes a task of make()'s step (made only if the stepper's free) and the steps after it: a ready one, if
    // their inner tasks all finish without waiting; otherwise a task of the stepper. A ready task of busy if a task of
    // the stepper's is under way (or what it makes, if it's a function). (What a step throws here comes out of
    // adapt().)
    template<typename Make, typename Busy>
        requires(std::is_invocable_r_v<step<In, Out>, Make>
                 && (std::is_invocable_r_v<Out, Busy> || std::is_constructible_v<Out, Busy>))
    task<Out> adapt(Make&& make, Busy&& busy)
    {
        if (this->is_claimed()) return ready_task<Out>(detail::busy_value<Out>(std::forward<Busy>(busy)));

        // (one left by a task of it that was dropped unawaited may hold what make() needs, e.g. a socket's write.)
        drop();

        step<In, Out> next = std::invoke(std::forward<Make>(make));
        while (auto* inner = std::get_if<task<In>>(&next))
        {
            if (!inner->finished_without_coroutine()) break;
            next = take_step(std::move(*inner).result());
        }
        if (std::holds_alternative<Out>(next)) return ready_task<Out>(std::get<Out>(std::move(next)));

        if (!driver) driver = detail::step_driver::drive(&stepper::advance, this);
        if (!this->try_claim()) return ready_task<Out>(detail::busy_value<Out>(std::forward<Busy>(busy)));

        hold(std::get<task<In>>(next));
        return operation_task<Out>(*this);
    }

    // NOLINTBEGIN(portability-template-virtual-member-function): overriders, which every compiler instantiates with
    // the class (they're in its vtable).
    bool start(std::coroutine_handle<> coroutine) noexcept override
    {
        awaiting = coroutine;
        return go();
    }

    Out result() override
    {
        auto taken = std::exchange(outcome, {});
        this->release();
        if constexpr (!std::is_same_v<decltype(Released), std::nullptr_t>) std::invoke(Released, *owner);

        if (const auto* thrown = std::get_if<std::exception_ptr>(&taken)) std::rethrow_exception(*thrown);
        return std::get<Out>(std::move(taken));
    }

    void cancel() noexcept override
    {
        if (op != nullptr) op->cancel();
        let_go_of_inner();
        this->release();
    }
    // NOLINTEND(portability-template-virtual-member-function)

private:
    step<In, Out> take_step(In&& value)
        requires(std::is_invocable_r_v<step<In, Out>, decltype(Step), Owner&, In&&>)
    {
        return std::invoke(Step, *owner, std::move(value));
    }

    // go starts the inner task held, for the driver to continue once it's finished, and takes the steps of those that
    // finish at once: it returns true if one waits (then nothing's touched after it's started, as it may resume the
    // driver, on another thread, at any moment), and false once the result's in.
    bool go() noexcept
    {
        while (true)
        {
            if (start_inner()) return true;
            if (take_steps()) return false;
        }
    }

    // advance is the driver's: an inner task has finished (see detail::step_driver).
    static std::coroutine_handle<> advance(void* self) noexcept
    {
        auto* s = static_cast<stepper*>(self);
        if (!s->take_steps() && s->go()) return std::noop_coroutine();
        return s->awaiting;
    }

    // start_inner starts the inner task held, returning whether it waits (see go()).
    bool start_inner() noexcept
    {
        if (op != nullptr) return op->start(driver.get());

        typename task<In>::awaitable_base handshake{handle, std::nullopt, nullptr};
        return handshake.await_suspend(driver.get());
    }

    // take_steps takes the finished inner task's result, and the steps after it, while their inner tasks finish without
    // waiting: it returns true once the result's in (or what a step threw), and false if it holds an inner task that
    // waits, unstarted.
    bool take_steps() noexcept
    {
        try
        {
            step<In, Out> next = take_step(take_inner());
            while (auto* inner = std::get_if<task<In>>(&next))
            {
                if (!inner->finished_without_coroutine())
                {
                    hold(*inner);
                    return false;
                }
                next = take_step(std::move(*inner).result());
            }
            outcome.template emplace<Out>(std::get<Out>(std::move(next)));
        }
        catch (...)
        {
            outcome = std::current_exception();
        }
        return true;
    }

    // take_inner takes the finished inner task's result (rethrowing what an inner coroutine threw), and lets go of it.
    In take_inner()
    {
        // NOTE: after the result's taken, which an inner coroutine's is a reference into its frame.
        const util::defer gone{[this] noexcept { let_go_of_inner(); }};
        if (op != nullptr) return op->result();
        return std::move(handle.promise()).result();
    }

    // hold makes the inner task's coroutine or operation the stepper's (only those, not the task, which is left empty:
    // a smaller stepper).
    void hold(task<In>& inner) noexcept
    {
        handle = std::exchange(inner.handle, nullptr);
        op     = std::exchange(inner.op, nullptr);
    }

    // drop lets go of an inner task that was never started (left by a task of the stepper dropped unawaited).
    void drop() noexcept
    {
        if (handle) std::exchange(handle, nullptr).destroy();
        if (op != nullptr) std::exchange(op, nullptr)->release();
    }

    // let_go_of_inner lets go of the inner task, which was started (its operation's done with, by result() or cancel();
    // its coroutine's frame is destroyed: one still waiting cancels what it waits for).
    void let_go_of_inner() noexcept
    {
        if (handle) std::exchange(handle, nullptr).destroy();
        op = nullptr;
    }

    Owner*                                                 owner;
    std::coroutine_handle<typename task<In>::promise_type> handle{nullptr};
    operation<In>*                                         op = nullptr;
    std::coroutine_handle<>                                awaiting{nullptr};
    std::variant<std::monostate, Out, std::exception_ptr>  outcome;
    detail::step_driver                                    driver;
};

}
