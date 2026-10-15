#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace net::coro
{

template<typename T>
class promise;

// task is a lazy coroutine: it starts when it's first co_awaited, and then resumes whatever awaited it when it finishes.
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
template<typename T = void>
class [[nodiscard]] task
{
public:
    using promise_type = promise<T>;
    using value_t      = T;

    struct awaitable_base
    {
        constexpr awaitable_base(std::coroutine_handle<promise_type> handle) noexcept
            : handle{handle}
        {}

        [[nodiscard]] bool await_ready() const noexcept { return !handle || handle.done(); }

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
        }

        return *this;
    }

    ~task()
    {
        if (handle) handle.destroy();
    }

    auto operator co_await() const& noexcept
    {
        struct awaitable : public awaitable_base
        {
            auto await_resume() { return this->handle.promise().result(); }
        };

        return awaitable{handle};
    }

    auto operator co_await() const&& noexcept
    {
        struct awaitable : public awaitable_base
        {
            auto await_resume() { return std::move(this->handle.promise()).result(); }
        };

        return awaitable{handle};
    }

    [[nodiscard]] bool resume() const noexcept
    {
        if (!handle.done()) handle.resume();
        return !handle.done();
    }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle); }

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

    [[nodiscard]] bool is_ready() const noexcept { return !handle || handle.done(); }

    [[nodiscard]] std::coroutine_handle<promise_type> get_handle() noexcept { return handle; }

    [[nodiscard]] promise_type&       get_promise() & { return handle.promise(); }
    [[nodiscard]] const promise_type& get_promise() const& { return handle.promise(); }
    [[nodiscard]] promise_type&&      get_promise() && { return std::move(handle.promise()); }

private:
    std::coroutine_handle<promise_type> handle;
};

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
    using stored_type = std::conditional_t<is_reference, std::remove_reference_t<T>, std::remove_const_t<T>>;

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
