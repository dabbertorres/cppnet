#pragma once

#include <coroutine>
#include <exception>
#include <iterator>
#include <memory>
#include <utility>
#include <variant>

namespace net::coro
{

template<std::movable T>
class generator
{
public:
    struct promise_type
    {
        std::variant<std::monostate, T, std::exception_ptr> value;

        generator get_return_object() { return generator{handle_type::from_promise(*this)}; }

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        template<std::convertible_to<T> From>
        std::suspend_always yield_value(From&& from)
        {
            value = std::forward<From>(from);
            return {};
        }

        void return_void() noexcept {}

        // disallow co_await in generator coroutines.
        /* void await_transform() = delete; */

        [[noreturn]] void unhandled_exception() { value = std::current_exception(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    // support range-based for loops
    class iterator
    {
    public:
        explicit iterator(handle_type handle)
            : handle{handle}
        {}

        void operator++() { handle.resume(); }

        T operator*() const { return handle.promise().value; }

        bool operator==(std::default_sentinel_t /*unused*/) const { return !handle || handle.done(); }

    private:
        handle_type handle;
    };

    explicit generator(handle_type h)
        : handle{h}
    {}

    generator(const generator&)            = delete;
    generator& operator=(const generator&) = delete;

    generator(generator&& other) noexcept
        : handle{std::exchange(other.handle, std::noop_coroutine())}
    {}

    generator& operator=(generator&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            if (handle) handle.destroy();
            handle = std::exchange(other.handle, std::noop_coroutine());
        }

        return *this;
    }

    ~generator()
    {
        if (handle) handle.destroy();
    }

    iterator begin()
    {
        if (handle) handle.resume();
        return iterator{handle};
    }

    std::default_sentinel_t end() { return {}; }

    explicit operator bool()
    {
        fill();
        return handle && !handle.done();
    }

    T operator()()
    {
        fill();
        full = false;
        // NOTE: will throw if value isn't T
        return std::move(std::get<T>(handle.promise().value));
    }

private:
    void fill()
    {
        if (!full)
        {
            handle.resume();
            if (std::holds_alternative<std::exception_ptr>(handle.promise().value))
                std::rethrow_exception(std::get<std::exception_ptr>(handle.promise().value));
            full = true;
        }
    }

    bool        full = false;
    handle_type handle;
};

}
