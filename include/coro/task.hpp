#pragma once

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

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
        {
            handle.promise().set_continuation(continuation);
            return handle;
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

    constexpr task& operator=(task&& other) noexcept
    {
        if (std::addressof(other) != this)
        {
            if (handle != nullptr) handle.destroy();

            handle = std::exchange(other.handle, nullptr);
        }

        return *this;
    }

    ~task()
    {
        if (handle != nullptr) handle.destroy();
    }

    operator task<>() const&& noexcept { return task{std::move(handle)}; }

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
        if (handle != nullptr)
        {
            handle.destroy();
            handle = nullptr;
            return true;
        }

        return false;
    }

    [[nodiscard]] bool is_ready() const noexcept { return handle == nullptr || handle.done(); }

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

private:
    friend struct final_awaitable;

    struct final_awaitable
    {
        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> coro) noexcept
        {
            auto& promise = coro.promise();
            if (promise.continuation != nullptr) return promise.continuation;
            return std::noop_coroutine();
        }

        void await_resume() noexcept { /* noop */ }
    };

    std::coroutine_handle<> continuation{nullptr};
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
