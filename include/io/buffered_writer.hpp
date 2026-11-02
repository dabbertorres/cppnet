#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "coro/adapter.hpp"
#include "coro/task.hpp"
#include "io.hpp"
#include "writer.hpp"

namespace net::io
{

class buffered_writer : public writer
{
public:
    explicit buffered_writer(writer* underlying, std::size_t bufsize = 1'024);

    coro::task<result> write(std::span<const std::byte> data) override;

    using writer::write;

    [[nodiscard]] handle native_handle() const noexcept override;

    // flush writes what's buffered. One flush (or write that doesn't fit in the buffer) at a time: another while one
    // waits fails with std::errc::device_or_resource_busy (writing nothing).
    coro::task<result> flush();

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] std::span<const std::byte> view() const noexcept;

    // prepare adds size bytes to the buffer, for the caller to write in place (e.g. formatting into it, rather than
    // into a string to write), and returns them; or returns none if they don't fit. They're written by the next flush.
    [[nodiscard]] std::span<std::byte> prepare(std::size_t size);

    // reset clears the buffer, and if other is not null, switches to it.
    // If other is null, the current writer is kept.
    // This allows re-use of the buffer's memory with other writers.
    void reset(writer* other);

    // reset clears the buffer.
    void reset();

private:
    // What writing's writing.
    enum class pending : std::uint8_t
    {
        flush,         // what's buffered (flush()).
        through_flush, // what's buffered, before through (a write that doesn't fit in the buffer).
        through,       // through, straight from it.
    };

    // write_through is write() of what doesn't fit in the buffer.
    // NOTE: not inlined into write(), whose usual case (what fits) stays small.
    [[gnu::noinline]] coro::task<result> write_through(std::span<const std::byte> data);

    // flush_waiting is flush(), once a write (inner, having written total before it) has to wait: writing's.
    // NOTE: not inlined into flush(), whose usual case (writes that don't wait) stays small.
    [[gnu::noinline]] coro::task<result> flush_waiting(coro::task<result> inner, std::size_t total);

    // wrote takes the result of a write that writing made, and makes the next step.
    coro::step<result, result> wrote(result res);

    // write_rest is the next step of a write that doesn't fit in the buffer, once what was buffered is written: a write
    // of what's wrapped for what's left of through, or (buffering what's left, if it fits) its result.
    coro::step<result, result> write_rest();

    // flushed drops the total written from the buffer, and returns flush()'s result, given the last write's.
    result flushed(std::size_t total, const result& last) noexcept;

    writer*                impl;
    std::vector<std::byte> buf;

    // writing writes what's buffered, and through, without a coroutine (wrote() taking each write's result): written of
    // what's buffered so far, and through_written of through.
    pending                                                                 what            = pending::flush;
    std::size_t                                                             written         = 0;
    std::span<const std::byte>                                              through         = {};
    std::size_t                                                             through_written = 0;
    coro::stepper<buffered_writer, result, result, &buffered_writer::wrote> writing{this};
};

}
