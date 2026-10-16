#pragma once

#include <cstddef>
#include <span>
#include <vector>

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

    [[nodiscard]] int native_handle() const noexcept override;

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
    // write_through writes what doesn't fit in the buffer, flushing it first.
    coro::task<result> write_through(std::span<const std::byte> data);

    // flush_after finishes flush(), once writing (which needed a coroutine) does, having written total before it.
    coro::task<result> flush_after(coro::task<result> writing, std::size_t total);

    // flushed drops the total written from the buffer, and returns flush()'s result, given the last write's.
    result flushed(std::size_t total, const result& last) noexcept;

    writer*                impl;
    std::vector<std::byte> buf;
};

}
