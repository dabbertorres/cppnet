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
    buffered_writer(writer* underlying, std::size_t bufsize = 1'024);

    result write(std::span<const std::byte> data) override;

    coro::task<result> co_write(std::span<const std::byte> data) override;

    using writer::write;

    [[nodiscard]] int native_handle() const noexcept override;

    result             flush();
    coro::task<result> co_flush();

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] std::span<const std::byte> view() const noexcept;

    // reset clears the buffer, and if other is not null, switches to it.
    // If other is null, the current writer is kept.
    // This allows re-use of the buffer's memory with other writers.
    void reset(writer* other);

    // reset clears the buffer.
    void reset();

private:
    result flush_available();

    writer*                impl;
    std::vector<std::byte> buf;
};

}
