#pragma once

#include <coroutine>
#include <cstddef>
#include <span>

#include "coro/task.hpp"
#include "io.hpp"
#include "io/event.hpp"

namespace net::io
{

class reader
{
public:
    reader(const reader&)                     = default;
    reader& operator=(const reader&) noexcept = default;

    reader(reader&&) noexcept            = default;
    reader& operator=(reader&&) noexcept = default;

    virtual ~reader() = default;

    virtual result read(std::span<std::byte> data) = 0;
    inline result  read(std::span<char> data) { return read(std::as_writable_bytes(data)); }
    inline result  read(std::byte& data) { return read({&data, 1}); }
    inline result  read(char& data) { return read({&data, 1}); }

    virtual coro::task<result> co_read(std::span<std::byte> data)
    {
        // awaitable for defaulting to a synchronous read
        struct awaitable
        {
            [[nodiscard]] constexpr bool await_ready() const noexcept { return true; }
            [[nodiscard]] result         await_resume() const { return r->read(data); }
            constexpr void await_suspend(std::coroutine_handle<promise> await_on) const noexcept { /* noop */ }

            reader*              r;
            std::span<std::byte> data;
        };

        auto r = co_await awaitable{this, data};
        co_return r;
    }

    inline coro::task<result> co_read(std::span<char> data) { return co_read(std::as_writable_bytes(data)); }
    inline coro::task<result> co_read(std::byte& data) { return co_read({&data, 1}); }
    inline coro::task<result> co_read(char& data) { return co_read({&data, 1}); }

    [[nodiscard]] virtual int native_handle() const noexcept = 0;

protected:
    reader() = default;
};

}
