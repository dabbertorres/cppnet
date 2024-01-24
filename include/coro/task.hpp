#pragma once

#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

namespace net::coro
{

namespace detail
{

    template<typename T>
    class promise;

}

template<typename T = void>
class [[nodiscard]] task
{
    struct awaitable_base;

public:
    using promise_type = detail::promise<T>;
    using value_t      = T;

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
            if (handle) handle.destroy();

            handle = std::exchange(other.handle, nullptr);
        }

        return *this;
    }

    ~task()
    {
        if (handle) handle.destroy();
    }

    auto operator co_await() const& noexcept;
    auto operator co_await() const&& noexcept;

    bool resume()
    {
        if (!handle.done()) handle.resume();
        return !handle.done();
    }

    /* bool destroy() */
    /* { */
    /*     if (handle != nullptr) */
    /*     { */
    /*         handle.destroy(); */
    /*         handle = nullptr; */
    /*         return true; */
    /*     } */

    /*     return false; */
    /* } */

    [[nodiscard]] bool is_ready() const noexcept { return !handle || handle.done(); }

    [[nodiscard]] bool when_ready() const noexcept;

    /* [[nodiscard]] promise_type&       get_promise() & { return handle.promise(); } */
    /* [[nodiscard]] const promise_type& get_promise() const& { return handle.promise(); } */
    /* [[nodiscard]] promise_type&&      get_promise() && { return std::move(handle.promise()); } */

    /* std::coroutine_handle<promise_type> get_handle() { return handle; } */

private:
    std::coroutine_handle<promise_type> handle;
};

template<typename T>
auto task<T>::operator co_await() const& noexcept
{
    struct awaitable : public awaitable_base
    {
        using awaitable_base::awaitable_base;

        auto await_resume()
        {
            if (!this->handle)
            {
                // TODO: throw
            }

            if constexpr (std::is_same_v<void, T>)
            {
                this->handle.promise().result();
                return;
            }

            return this->handle.promise().result();
        }
    };

    return awaitable{handle};
}

template<typename T>
auto task<T>::operator co_await() const&& noexcept
{
    struct awaitable : public awaitable_base
    {
        using awaitable_base::awaitable_base;

        auto await_resume()
        {
            if (!this->handle)
            {
                // TODO: throw
            }

            if constexpr (std::is_same_v<void, T>)
            {
                this->handle.promise().result();
                return;
            }

            return std::move(this->handle.promise()).result();
        }
    };

    return awaitable{handle};
}

template<typename T>
[[nodiscard]] bool task<T>::when_ready() const noexcept
{
    struct awaitable : public awaitable_base
    {
        using awaitable_base::awaitable_base;

        void await_resume() const noexcept {}
    };

    return awaitable{handle};
}

template<typename T>
struct task<T>::awaitable_base
{
    constexpr awaitable_base(std::coroutine_handle<detail::promise<T>> handle) noexcept
        : handle{handle}
    {}

    [[nodiscard]] bool await_ready() const noexcept { return !handle || handle.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
    {
        handle.promise().set_continuation(continuation);
        return handle;
    }

    std::coroutine_handle<promise_type> handle;
};

}

namespace net::coro::detail
{

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
            return coro.promise().continuation;
        }

        void await_resume() noexcept {}
    };

    std::coroutine_handle<> continuation;
};

template<typename T>
class promise final : public promise_base
{
public:
    task<T> get_return_object() noexcept { return task<T>{std::coroutine_handle<promise<void>>::from_promise(*this)}; }

    void return_value(T new_value) noexcept { value = new_value; }
    void return_value(T&& new_value) noexcept { value = std::move(new_value); }

    void unhandled_exception() noexcept { value = std::current_exception(); }

    [[nodiscard]] const T& result() const&
    {
        if (std::holds_alternative<std::exception_ptr>(value))
            std::rethrow_exception(std::get<std::exception_ptr>(value));

        return std::get<T>(value);
    }

    [[nodiscard]] T&& result() &&
    {
        if (std::holds_alternative<std::exception_ptr>(value))
            std::rethrow_exception(std::get<std::exception_ptr>(value));

        return std::move(std::get<T>(value));
    }

private:
    std::variant<std::monostate, T, std::exception_ptr> value;
};

template<typename T>
class promise<T&> final : public promise_base
{
private:
    using bare_value_t = std::remove_reference_t<T>;
    using value_t      = std::reference_wrapper<bare_value_t>;

public:
    task<T&> get_return_object() noexcept
    {
        return task<T&>{std::coroutine_handle<promise<void>>::from_promise(*this)};
    }

    void return_value(T& new_value) noexcept { value = new_value; }

    void unhandled_exception() noexcept { value = std::current_exception(); }

    [[nodiscard]] T& result() const&
    {
        if (std::holds_alternative<std::exception_ptr>(value))
            std::rethrow_exception(std::get<std::exception_ptr>(value));

        return std::get<value_t>(value);
    }

private:
    std::variant<std::monostate, value_t, std::exception_ptr> value;
};

template<>
class promise<void> final : public promise_base
{
public:
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
