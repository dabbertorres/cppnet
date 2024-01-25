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

    virtual result read(std::span<std::byte> data) = 0;
    inline result  read(std::span<char> data) { return read(std::as_writable_bytes(data)); }
    inline result  read(std::byte& data) { return read({&data, 1}); }
    inline result  read(char& data) { return read({&data, 1}); }

    /* virtual coro::task<io::result> co_read(std::span<std::byte> data) = 0; */

    /* inline coro::task<io::result> co_read(std::span<char> data) { return co_read(std::as_writable_bytes(data)); } */

    [[nodiscard]] virtual int native_handle() const noexcept = 0;

protected:
    reader() = default;
};

}
