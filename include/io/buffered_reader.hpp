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
    buffered_reader(reader& impl, size_t bufsize = 1024);

    result read(byte* data, size_t length) override;

    using reader::read;

    // peek sets next to the next byte and returns true, if available.
    //
    // If no next byte is available, next is not modified, and false is returned.
    //
    // NOTE: peek does not implicitly read more data, instead false if none is immediately available.
    //       See ensure if you want to make sure data is available.
    [[nodiscard]] std::tuple<byte, bool> peek();

    [[nodiscard]] size_t capacity() const noexcept { return buf.capacity(); }
    [[nodiscard]] size_t size() const noexcept { return buf.size(); }

    // reset clears the buffer, and if other is not null, switches to it.
    // If other is null, the current reader is kept.
    // This allows re-use of the buffer's memory with other readers.
    void reset(reader* other);

    // reset clears the buffer.
    void reset();

    // error returns the current error, if any.
    // Potentially useful if e.g. peek() fails.
    [[nodiscard]] std::error_condition error() const;

private:
    void fill();

    reader*              impl;
    std::vector<byte>    buf;
    std::error_condition err;
};

}
