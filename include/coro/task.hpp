#pragma once

#include <atomic>
#include <coroutine> // IWYU pragma: export (coroutines need it, wherever they use these)
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace net::coro
{

template<typename T>
class promise;

// task is a lazy coroutine: it starts when it's first co_awaited, and then resumes whatever awaited it when it
// finishes.
//
// How co_await hands over to a task (and back), and why:
//
// The usual way is symmetric transfer: await_suspend returns the task's handle (so the awaiting coroutine suspends, and
// the task resumes), and the task's final_suspend returns the awaiting coroutine's handle (so it resumes). That's
// cheapest, but it only keeps the stack from growing if the compiler makes those transfers tail calls. Otherwise, a
// task that finishes without suspending (e.g. a read of what's already buffered) resumes its awaiter from inside its
// own frame, which is inside the awaiter's frame, and so on: a loop of such co_awaits grows the stack until it
// overflows. Clang makes them tail calls even without optimizations (as far as has been tested: x86-64 and arm64), but
// GCC only does when optimizing, and not with ASan (which the debug presets use).
//
// So instead, await_suspend resumes the task directly, and only suspends the awaiting coroutine if the task suspended:
// one that finishes straight away just returns to await_suspend, which then lets the awaiting coroutine carry on in its
// own frame. A task that suspends (to wait for I/O) and finishes later, on another thread, resumes the awaiting
// coroutine itself. An atomic flag in the promise decides which side continues the awaiting coroutine, as the task
// may finish (on another thread) at any moment after it suspends.
//
// That's one code path for all compilers, which keeps the stack's depth independent of the compiler and its flags, at
// the cost of an atomic exchange per co_await: about 6-10ns more than symmetric transfer, for a co_await of a task that
// finishes straight away (e.g. 12 -> 18ns with Clang, 49 -> 60ns with GCC, on x86-64 Linux, where allocating the
// task's frame is most of the rest). That's small next to I/O (a recv() is microseconds), but bench/coro/task.cpp
// measures both ways, to check again later.
//
// A task may also be made already finished, with its result (ready_task()), without a coroutine: calling a coroutine
// allocates its frame (before it even starts), so a function that can often finish without waiting (e.g. a read of
// what's already buffered) returns one of those, and only calls a coroutine when it must wait. Awaiting it just takes
// the result.
template<typename T = void>
class [[nodiscard]] task
{
public:
    using promise_type = promise<T>;
    using value_t      = T;

    // ready_type holds a ready task's result: a pointer for a reference, and nothing for void.
    using ready_type = std::conditional_t<std::is_void_v<T>,
                                          std::monostate,
                                          std::conditional_t<std::is_reference_v<T>,
                                                             std::add_pointer_t<std::remove_reference_t<T>>,
                                                             std::remove_const_t<T>>>;

    // NOTE: the awaiter holds the handle (and a ready task's result), rather than the task: then the compiler can see
    //       that the handle doesn't escape, and may allocate the coroutine's frame in the awaiting coroutine's (HALO).
    struct awaitable_base
    {
        constexpr awaitable_base(std::coroutine_handle<promise_type> handle, std::optional<ready_type> ready) noexcept
            : handle{handle}
            , ready{std::move(ready)}
        {}

        [[nodiscard]] bool await_ready() const noexcept { return ready.has_value() || !handle || handle.done(); }

        // await_suspend starts the task, and only suspends the awaiting coroutine if the task suspended (see task's
        // comment, for why not symmetric transfer).
        bool await_suspend(std::coroutine_handle<> continuation) noexcept
        {
            auto& promise = handle.promise();
            promise.set_continuation(continuation);

            handle.resume();

            // usually, it's already finished: then a load is enough to know it, rather than an exchange.
            if (promise.finished()) return false;

            // otherwise, whichever of this and the task's final_suspend comes second continues the awaiting coroutine.
            return !promise.mark_awaiter_or_finished();
        }

        std::coroutine_handle<promise_type> handle{nullptr};
        std::optional<ready_type>           ready;
    };

    constexpr task() noexcept
        : handle{nullptr}
    {}

    explicit constexpr task(std::coroutine_handle<promise_type> coroutine)
        : handle{coroutine}
    {}

    task(const task&)            = delete;
    task& operator=(const task&) = delete;

    constexpr task(task&& other) noexcept
        : handle{std::exchange(other.handle, nullptr)}
        , ready{std::exchange(other.ready, std::nullopt)}
    {}

    // NOTE: handle is tested with operator bool rather than compared to nullptr. A comparison looks up operator== in
    //       the namespaces of T (via the promise type), which can find e.g. std::expected's unconstrained operator==
    //       and recurse forever.

    constexpr task& operator=(task&& other) noexcept
    {
        if (std::addressof(other) != this)
        {
            if (handle) handle.destroy();

            handle = std::exchange(other.handle, nullptr);
            ready  = std::exchange(other.ready, std::nullopt);
        }

        return *this;
    }

    ~task()
    {
        if (handle) handle.destroy();
    }

    auto operator co_await() & noexcept
    {
        struct awaitable : public awaitable_base
        {
            T await_resume()
            {
                if (this->ready.has_value()) return task::from_ready(std::move(*this->ready));
                return this->handle.promise().result();
            }
        };

        return awaitable{
            awaitable_base{handle, ready}
        };
    }

    auto operator co_await() && noexcept
    {
        struct awaitable : public awaitable_base
        {
            T await_resume()
            {
                if (this->ready.has_value()) return task::from_ready(std::move(*this->ready));
                return std::move(this->handle.promise()).result();
            }
        };

        return awaitable{
            awaitable_base{handle, std::move(ready)}
        };
    }

    [[nodiscard]] bool resume() const noexcept
    {
        if (ready.has_value()) return false;
        if (!handle.done()) handle.resume();
        return !handle.done();
    }

    [[nodiscard]] bool valid() const noexcept { return ready.has_value() || static_cast<bool>(handle); }

    bool destroy()
    {
        if (handle)
        {
            handle.destroy();
            handle = nullptr;
            return true;
        }

        return false;
    }

    [[nodiscard]] bool is_ready() const noexcept { return ready.has_value() || !handle || handle.done(); }

    // finished_without_coroutine is whether this is a ready task (see ready_task()): its result can be taken with
    // result() right away.
    [[nodiscard]] bool finished_without_coroutine() const noexcept { return ready.has_value(); }

    // result is the finished task's result (rethrowing its exception, if it threw).
    T result() &&
    {
        if (ready.has_value()) return from_ready(std::move(*ready));
        return std::move(handle.promise()).result();
    }

    // get_handle is the coroutine's handle: null for a ready task, which has none.
    [[nodiscard]] std::coroutine_handle<promise_type> get_handle() noexcept { return handle; }

    [[nodiscard]] promise_type&       get_promise() & { return handle.promise(); }
    [[nodiscard]] const promise_type& get_promise() const& { return handle.promise(); }
    [[nodiscard]] promise_type&&      get_promise() && { return std::move(handle.promise()); }

    template<typename U, typename... Args>
    friend task<U> ready_task(Args&&... args);

private:
    // from_ready is the result a ready task holds.
    static T from_ready(ready_type value)
    {
        if constexpr (std::is_void_v<T>) return;
        else if constexpr (std::is_reference_v<T>) return static_cast<T>(*value);
        else return std::move(value);
    }

    std::coroutine_handle<promise_type> handle;
    std::optional<ready_type>           ready;
};

// ready_task makes a task that has already finished, with its result made of args (none, for task<void>), without a
// coroutine (see task).
template<typename T = void, typename... Args>
task<T> ready_task(Args&&... args)
{
    task<T> out;
    if constexpr (std::is_reference_v<T>)
    {
        static_assert(sizeof...(Args) == 1, "a reference is made of one argument");
        T ref = (std::forward<Args>(args), ...);
        out.ready.emplace(std::addressof(ref));
    }
    else
    {
        out.ready.emplace(std::forward<Args>(args)...);
    }
    return out;
}

class promise_base
{
public:
    promise_base() noexcept = default;

    auto initial_suspend() noexcept { return std::suspend_always{}; }
    auto final_suspend() noexcept { return final_awaitable{}; }

    void set_continuation(std::coroutine_handle<> handle) noexcept { continuation = handle; }

    // mark_awaiter_or_finished is called by the task as it finishes, and by the awaiting coroutine (once it's started
    // the task) unless it's already seen the task finish. It returns true for whichever comes second, which is then
    // the one to continue the awaiting coroutine: the awaiter by not suspending, or the task by resuming it.
    bool mark_awaiter_or_finished() noexcept { return second.exchange(true, std::memory_order::acq_rel); }

    // finished returns whether the task has finished, if the awaiting coroutine hasn't called mark_awaiter_or_finished
    // yet (it's the only other thing that sets the flag).
    [[nodiscard]] bool finished() const noexcept { return second.load(std::memory_order::acquire); }

private:
    friend struct final_awaitable;

    struct final_awaitable
    {
        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> coro) noexcept
        {
            auto& promise = coro.promise();

            // the awaiting coroutine is still in its await_suspend (or nothing awaits this task): returning there
            // continues it.
            if (!promise.mark_awaiter_or_finished() || promise.continuation == nullptr) return std::noop_coroutine();

            // it suspended, waiting for this task.
            return promise.continuation;
        }

        void await_resume() noexcept { /* noop */ }
    };

    std::coroutine_handle<> continuation{nullptr};
    std::atomic<bool>       second{false};
};

template<typename T>
class promise final : public promise_base
{
public:
    static constexpr bool is_reference = std::is_reference_v<T>;
    // NOTE: a reference is stored as a pointer.
    using stored_type =
        std::conditional_t<is_reference, std::add_pointer_t<std::remove_reference_t<T>>, std::remove_const_t<T>>;

    promise() noexcept                 = default;
    promise(const promise&)            = delete;
    promise(promise&&)                 = delete;
    promise& operator=(const promise&) = delete;
    promise& operator=(promise&&)      = delete;
    ~promise()                         = default;

    task<T> get_return_object() noexcept { return task<T>{std::coroutine_handle<promise<T>>::from_promise(*this)}; }

    template<typename Y>
        requires((is_reference && std::is_constructible_v<T, Y &&>)
                 || (!is_reference && std::is_constructible_v<stored_type, Y &&>))
    void return_value(Y&& new_value) noexcept
    {
        if constexpr (is_reference)
        {
            T ref = static_cast<Y&&>(new_value);
            value.template emplace<stored_type>(std::addressof(ref));
        }
        else
        {
            value.template emplace<stored_type>(std::forward<Y>(new_value));
        }
    }

    void return_value(stored_type new_value) noexcept
        requires(!is_reference)
    {
        if constexpr (std::is_move_constructible_v<stored_type>)
        {
            value.template emplace<stored_type>(std::move(new_value));
        }
        else
        {
            value.template emplace<stored_type>(new_value);
        }
    }

    void unhandled_exception() noexcept { value = std::current_exception(); }

    [[nodiscard]] T result() &
    {
        if (std::holds_alternative<stored_type>(value))
        {
            if constexpr (is_reference) return static_cast<T>(*std::get<stored_type>(value));
            else return static_cast<const T&>(std::get<stored_type>(value));
        }
        else if (std::holds_alternative<std::exception_ptr>(value))
        {
            std::rethrow_exception(std::get<std::exception_ptr>(value));
        }
        else
        {
            throw std::runtime_error{"return value never set, coroutine may have never been executed"};
        }
    }

    [[nodiscard]] const T& result() const&
    {
        if (std::holds_alternative<stored_type>(value))
        {
            if constexpr (is_reference) return static_cast<std::add_const_t<T>>(*std::get<stored_type>(value));
            else return static_cast<const T&>(std::get<stored_type>(value));
        }
        else if (std::holds_alternative<std::exception_ptr>(value))
        {
            std::rethrow_exception(std::get<std::exception_ptr>(value));
        }
        else
        {
            throw std::runtime_error{"return value never set, coroutine may have never been executed"};
        }
    }

    [[nodiscard]] T&& result() &&
    {
        if (std::holds_alternative<stored_type>(value))
        {
            if constexpr (is_reference) return static_cast<T>(*std::get<stored_type>(value));
            else if constexpr (std::is_move_constructible_v<T>) return static_cast<T&&>(std::get<stored_type>(value));
            else return static_cast<const T&&>(std::get<stored_type>(value));
        }
        else if (std::holds_alternative<std::exception_ptr>(value))
        {
            std::rethrow_exception(std::get<std::exception_ptr>(value));
        }
        else
        {
            throw std::runtime_error{"return value never set, coroutine may have never been executed"};
        }
    }

private:
    std::variant<std::monostate, stored_type, std::exception_ptr> value;
};

template<>
class promise<void> final : public promise_base
{
public:
    promise() noexcept                 = default;
    promise(const promise&)            = delete;
    promise(promise&&)                 = delete;
    promise& operator=(const promise&) = delete;
    promise& operator=(promise&&)      = delete;
    ~promise()                         = default;

    task<void> get_return_object() noexcept
    {
        return task<void>{std::coroutine_handle<promise<void>>::from_promise(*this)};
    }

    void return_void() noexcept {}

    void unhandled_exception() noexcept { exception = std::current_exception(); }

    void result()
    {
        if (exception != nullptr) std::rethrow_exception(exception);
    }

private:
    std::exception_ptr exception;
};

}
