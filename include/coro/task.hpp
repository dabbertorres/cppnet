#pragma once

#include <coroutine>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace net::coro
{

template<typename T>
class promise;

template<typename T = void>
class [[nodiscard]] task
{
    struct awaitable_base;

public:
    using promise_type = promise<T>;
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
            if (handle != nullptr) handle.destroy();

            handle = std::exchange(other.handle, nullptr);
        }

        return *this;
    }

    ~task()
    {
        if (handle != nullptr) handle.destroy();
    }

    auto operator co_await() const& noexcept;
    auto operator co_await() const&& noexcept;

    bool resume()
    {
        if (!handle.done()) handle.resume();
        return !handle.done();
    }

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

    [[nodiscard]] bool when_ready() const noexcept;

    [[nodiscard]] promise_type&       get_promise() & { return handle.promise(); }
    [[nodiscard]] const promise_type& get_promise() const& { return handle.promise(); }
    [[nodiscard]] promise_type&&      get_promise() && { return std::move(handle.promise()); }

    std::coroutine_handle<promise_type> get_handle() { return handle; }

private:
    std::coroutine_handle<promise_type> handle;
};

template<typename T>
auto task<T>::operator co_await() const& noexcept
{
    struct awaitable : awaitable_base
    {
        using awaitable_base::awaitable_base;

        decltype(auto) await_resume()
        {
            if constexpr (std::is_same_v<void, T>)
            {
                handle.promise().result();
                return;
            }

            return handle.promise().result();
        }
    };

    return awaitable{handle};
}

template<typename T>
auto task<T>::operator co_await() const&& noexcept
{
    struct awaitable : awaitable_base
    {
        using awaitable_base::awaitable_base;

        decltype(auto) await_resume()
        {
            if constexpr (std::is_same_v<void, T>)
            {
                handle.promise().result();
                return;
            }

            return std::move(handle.promise()).result();
        }
    };

    return awaitable{handle};
}

template<typename T>
[[nodiscard]] bool task<T>::when_ready() const noexcept
{
    struct awaitable : awaitable_base
    {
        using awaitable_base::awaitable_base;

        void await_resume() const noexcept {}
    };

    return awaitable{handle};
}

template<typename T>
struct task<T>::awaitable_base
{
    constexpr awaitable_base(std::coroutine_handle<> handle) noexcept
        : handle{handle}
    {}

    [[nodiscard]] bool await_ready() const noexcept { return handle == nullptr || handle.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept
    {
        handle.promise().set_continuation(continuation);
        return handle;
    }

    std::coroutine_handle<promise_type> handle;
};

}
