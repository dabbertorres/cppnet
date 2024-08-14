#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include "coro/task.hpp"

#include "io.hpp"
#include "reader.hpp"

namespace net::io
{

class buffered_reader : public reader
{
public:
    buffered_reader(reader* impl, std::size_t bufsize = 1'024);

    result read(std::span<std::byte> data) override;

    coro::task<result> co_read(std::span<std::byte> data) override;

    using reader::co_read;
    using reader::read;

    struct read_until_result
    {
        std::span<std::byte> data;
        bool                 is_prefix;
        std::error_condition err;
    };

    // read_until returns a span of the data in the buffer up to (but not including)
    // delim.
    // If the buffer fills up before delim is found, then the partial data is returned,
    // and is_prefix will be true.
    // If an error occurs while reading to look for delim, err is set.
    read_until_result read_until(std::span<const std::byte> delim) noexcept;
    read_until_result read_until(std::string_view delim) noexcept
    {
        return read_until(std::as_bytes(std::span{delim.begin(), delim.end()}));
    }

    // peek sets next to the next byte and returns true, if available.
    //
    // If no next byte is available, next is not modified, and false is returned.
    [[nodiscard]] std::tuple<std::byte, bool> peek();
    coro::task<std::tuple<std::byte, bool>>   co_peek();

    [[nodiscard]] std::size_t capacity() const noexcept { return buf.capacity(); }
    [[nodiscard]] std::size_t size() const noexcept { return buf.size(); }

    // reset clears the buffer, and if other is not null, switches to it.
    // If other is null, the current reader is kept.
    // This allows re-use of the buffer's memory with other readers.
    void reset(reader* other);

    // reset clears the buffer.
    void reset();

    // error returns the current error, if any.
    // Potentially useful if e.g. peek() fails.
    [[nodiscard]] std::error_condition error() const;

    [[nodiscard]] int native_handle() const noexcept override { return impl->native_handle(); }

private:
    coro::task<void> fill();

    reader*                impl;
    std::vector<std::byte> buf;
    std::error_condition   err;
};

}
