#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <variant>

#include "coro/task.hpp"

namespace net::coro
{

template<typename T>
class promise;

class promise_base
{
public:
    auto initial_suspend() noexcept { return std::suspend_always{}; }
    auto final_suspend() noexcept { return final_awaitable{}; }

    void unhandled_exception() noexcept { exception = std::current_exception(); }

    void set_continuation(std::coroutine_handle<> continuation) noexcept { handle = continuation; }

private:
    friend struct final_awaitable;

    struct final_awaitable
    {
        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> coro) noexcept
        {
            auto& promise = coro.promise();
            if (promise.handle != nullptr) return promise.handle;
            return std::noop_coroutine();
        }

        void await_resume() noexcept {}
    };

protected:
    std::coroutine_handle<> handle;
    std::exception_ptr      exception;
};

template<typename T>
    requires(!std::is_lvalue_reference_v<T>)
class promise<T> final : public promise_base
{
public:
    using task_t                       = task<T>;
    static constexpr bool is_reference = false;

    task_t get_return_object() noexcept { return task_t{std::coroutine_handle<promise<T>>::from_promise(*this)}; }

    void return_value(T new_value) noexcept { value = std::move(new_value); }

    [[nodiscard]] const T& result() const&
    {
        if (exception != nullptr) std::rethrow_exception(exception);

        return value;
    }

    [[nodiscard]] T&& result() &&
    {
        if (exception != nullptr) std::rethrow_exception(exception);

        return std::move(value);
    }

private:
    // TODO: consider replacing with a std::variant<std::monostate, T, std::exception_ptr>
    T value;
};

template<typename T>
    requires(std::is_lvalue_reference_v<T>)
class promise<T> final : public promise_base
{
private:
    using bare_value_t = std::remove_reference_t<T>;
    using value_t      = std::optional<std::reference_wrapper<bare_value_t>>;

public:
    using task_t                       = task<T>;
    static constexpr bool is_reference = true;

    task_t get_return_object() noexcept { return task_t{std::coroutine_handle<promise<T>>::from_promise(*this)}; }

    void return_value(T new_value) noexcept
    {
        value.reset();
        value = new_value;
    }

    [[nodiscard]] T result() const&
    {
        if (exception != nullptr) std::rethrow_exception(exception);

        return value.value();
    }

private:
    // TODO: consider replacing with a std::variant<std::monostate, T, std::exception_ptr>
    value_t value;
};

template<>
class promise<void> : public promise_base
{
public:
    using task_t = task<void>;

    task_t get_return_object() noexcept { return task_t{std::coroutine_handle<promise<void>>::from_promise(*this)}; }

    void return_void() noexcept {}

    void result()
    {
        if (exception != nullptr) std::rethrow_exception(exception);
    }
};

}
