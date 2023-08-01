#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <vector>

#include "io.hpp"
#include "reader.hpp"

namespace net::io
{

class buffered_reader : public reader
{
public:
    buffered_reader(reader* impl, std::size_t bufsize = 1'024);

    result read(std::byte* data, std::size_t length) override;

    using reader::read;

    // peek sets next to the next byte and returns true, if available.
    //
    // If no next byte is available, next is not modified, and false is returned.
    //
    // NOTE: peek does not implicitly read more data, instead false if none is immediately available.
    //       See ensure if you want to make sure data is available.
    [[nodiscard]] std::tuple<std::byte, bool> peek();

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
    void fill();

    reader*                impl;
    std::vector<std::byte> buf;
    std::error_condition   err;
};

}
