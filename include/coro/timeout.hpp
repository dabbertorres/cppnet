#pragma once

#include <coroutine>
#include <type_traits>

#include "util/thread_pool.hpp"

namespace net::coro
{

template<typename T>
    requires(!std::is_void_v<T> && !std::is_reference_v<T>)
class timeout : std::future<T>
{
public:
    struct promise_type : std::promise<T>
    {
        std::future<T> get_return_object() noexcept { return this->get_future(); }

        [[nodiscard]] std::suspend_never initial_suspend() const noexcept { return {}; }
        [[nodiscard]] std::suspend_never final_suspend() const noexcept { return {}; }

        void return_value(const T& value) noexcept(std::is_nothrow_copy_constructible_v<T>) { set_value(value); }
        void return_value(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>) { set_value(std::move(value)); }

        void unhandled_exception() noexcept { this->set_exception(std::current_exception()); }
    };

    timeout(util::thread_pool& pool)
        : pool{&pool}
    {}

    [[nodiscard]] bool await_ready() const noexcept { return this->wait_for() != std::future_status::timeout; }

    void await_suspend(std::coroutine_handle<promise_type> handle)
    {
        pool->schedule(
            [this, handle]
            {
                this->wait();
                handle();
            });
    }

    T await_resume() { return this->get(); }

private:
    util::thread_pool* pool;
};

}
