#pragma once

#include <cstddef>
#include <span>

#include "coro/task.hpp"
#include "io.hpp"

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

    virtual coro::task<result> read(std::span<std::byte> data) = 0;
    inline coro::task<result>  read(std::span<char> data) { return read(std::as_writable_bytes(data)); }
    inline coro::task<result>  read(std::byte& data) { return read({&data, 1}); }
    inline coro::task<result>  read(char& data) { return read({&data, 1}); }

    [[nodiscard]] virtual int native_handle() const noexcept = 0;

protected:
    reader() = default;
};

}
