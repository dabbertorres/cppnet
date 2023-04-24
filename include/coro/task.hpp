#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace net::coro
{

template<typename T = void>
class [[nodiscard]] task
{
    struct awaitable_base;
    class promise_base;

public:
    class promise_type;
    using value_type = T;

    task() noexcept
        : handle(nullptr)
    {}

    explicit task(std::coroutine_handle<promise_type> coroutine)
        : handle(coroutine)
    {}

    task(const task&)            = delete;
    task& operator=(const task&) = delete;

    task(task&& other) noexcept
        : handle(other.coroutine)
    {
        other.handle = nullptr;
    }

    task& operator=(task&& other) noexcept
    {
        if (std::addressof(other) != this)
        {
            if (handle != nullptr) handle.destroy();

            handle       = other.handle;
            other.handle = nullptr;
        }

        return *this;
    }

    ~task()
    {
        if (handle != nullptr) handle.destroy();
    }

    [[nodiscard]] bool is_ready() const noexcept { return handle == nullptr || handle.done(); }

    auto operator co_await() const& noexcept
    {
        struct awaitable : awaitable_base
        {
            using awaitable_base::awaitable_base;

            decltype(auto) await_resume()
            {
                if (handle == nullptr) throw std::runtime_error("broken promise"); // TODO: use specific exception type

                return handle.promise().result();
            }
        };

        return awaitable{handle};
    }

    auto operator co_await() const&& noexcept
    {
        struct awaitable : awaitable_base
        {
            using awaitable_base::awaitable_base;

            decltype(auto) await_resume()
            {
                if (handle == nullptr) throw std::runtime_error("broken promise"); // TODO: use specific exception type

                return std::move(handle.promise()).result();
            }
        };

        return awaitable{handle};
    }

    [[nodiscard]] bool when_ready() const noexcept
    {
        struct awaitable : awaitable_base
        {
            using awaitable_base::awaitable_base;

            void await_resume() const noexcept {}
        };

        return awaitable(handle);
    }

private:
    std::coroutine_handle<promise_type> handle;
};

template<typename T>
struct task<T>::awaitable_base
{
    std::coroutine_handle<promise_type> handle;

    [[nodiscard]] bool await_ready() const noexcept { return handle == nullptr || handle.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept
    {
        handle.promise().set_continuation(awaiting);
        return handle;
    }

    decltype(auto) await_resume()
    {
        if (handle == nullptr) throw std::runtime_error("broken promise"); // TODO: use specific exception type

        return std::move(handle.promise()).result();
    }
};

template<typename T>
class task<T>::promise_base
{
    friend struct final_awaitable;

    struct final_awaitable
    {
        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> coro) noexcept
        {
            return coro.promise().handle;
        }

        void await_resume() noexcept {}
    };

public:
    promise_base() noexcept = default;

    auto initial_suspend() noexcept { return std::suspend_always{}; }
    auto final_suspend() noexcept { return final_awaitable{}; }

    void set_continuation(std::coroutine_handle<> continuation) noexcept { this->handle = continuation; }

private:
    std::coroutine_handle<> handle;
};

template<typename T>
class task<T>::promise_type final : public promise_base
{
public:
    promise_type() noexcept = default;

    promise_type(const promise_type&)            = default;
    promise_type& operator=(const promise_type&) = default;

    promise_type(promise_type&&) noexcept            = default;
    promise_type& operator=(promise_type&&) noexcept = default;

    ~promise_type()
    {
        std::visit(state,
                   [](auto&& arg)
                   {
                       using V = std::decay_t<decltype(arg)>;
                       arg.~V();
                   });
    }

    task<T> get_return_object() noexcept;

    void unhandled_exception() noexcept { state.emplace(std::current_exception()); }

    template<typename V>
        requires std::convertible_to<V&&, T>
    void return_value(V&& value) noexcept(std::is_nothrow_constructible_v<T, V&&>)
    {
        state.emplace(std::forward<V>(value));
    }

    T& result() &
    {
        if (const auto* ex = std::get_if<std::exception_ptr>(&state)) std::rethrow_exception(*ex);

        return std::get<T>(state);
    }

    T&& result() &&
    {
        if (const auto* ex = std::get_if<std::exception_ptr>(&state)) std::rethrow_exception(*ex);

        return std::move(std::get<T>(state));
    }

private:
    std::variant<T, std::exception_ptr> state;
};

template<>
class task<void>::promise_type : public promise_base
{
public:
    promise_type() noexcept = default;

    task<void> get_return_object() noexcept;

    void return_void() noexcept {}

    void unhandled_exception() noexcept { exception = std::current_exception(); }

    void result()
    {
        if (exception) std::rethrow_exception(exception);
    }

private:
    std::exception_ptr exception;
};

/* template<typename T> */
/* class task<T>::promise_type : public promise_base */
/* {}; */

}
