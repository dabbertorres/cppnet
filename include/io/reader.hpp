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

    // read reads up to data.size() bytes into data, returning how many were read.
    //
    // A read into a non-empty span returns at least one byte, or an error. The end of the stream is reported as
    // errc::end_of_stream, possibly along with the final bytes. (To check for that or the connection's being reset, as
    // most callers treat both as the end, compare the error with status_condition::closed.) A read into an empty span
    // returns 0 bytes and no error.
    virtual coro::task<result> read(std::span<std::byte> data) = 0;
    coro::task<result>         read(std::span<char> data) { return read(std::as_writable_bytes(data)); }
    coro::task<result>         read(std::byte& data) { return read({&data, 1}); }
    coro::task<result>         read(char& data) { return read({&data, 1}); }

    [[nodiscard]] virtual handle native_handle() const noexcept = 0;

protected:
    reader() = default;
};

}
