#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <future>
#include <iterator>
#include <type_traits>

namespace net::coro
{

template<std::movable T>
class generator
{
public:
    struct promise_type
    {
        T value;

        generator get_return_object() { return generator{handle_type::from_promise(*this)}; }

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        template<std::convertible_to<T> From>
        std::suspend_always yield_value(From&& from)
        {
            value = std::forward<From>(from);
            return {};
        }

        // disallow co_await in generator coroutines.
        void await_transform() = delete;

        [[noreturn]] static void unhandled_exception() { throw; }
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
        : handle{other.handle}
    {
        other.handle = {};
    }

    generator& operator=(generator&& other) noexcept
    {
        if (this != &other)
        {
            if (handle) handle.destroy();
            handle       = other.handle;
            other.handle = {};
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
        return std::move(handle.promise().value);
    }

private:
    void fill()
    {
        if (!full)
        {
            handle.resume();
            full = true;
        }
    }

    bool        full = false;
    handle_type handle;
};

}
