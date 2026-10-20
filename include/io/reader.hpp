#pragma once

#include <cstddef>
#include <span>
#include <system_error>

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

// end_callback is called by a reader that reads part of a stream (e.g. a message's body), once it has read to the end
// of its part: to tell its owner, without a coroutine, or anything allocated.
struct end_callback
{
    void (*call)(void* context) noexcept = nullptr;
    void* context                        = nullptr;

    void operator()() const noexcept
    {
        if (call != nullptr) call(context);
    }
};

// start_callback is awaited by a reader that reads part of a stream (e.g. a message's body) before its first read (into
// a non-empty span) reads anything: to tell its owner, which may have to write something first (e.g. HTTP's 100
// Continue, which a client waits for before it sends the body). An error it returns fails that read. It's a task (a
// coroutine only when it must wait), as it's for what's rare: an end_callback is for what's common.
struct start_callback
{
    coro::task<std::error_code> (*call)(void* context) = nullptr;
    void* context                                      = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept { return call != nullptr; }
};

}
