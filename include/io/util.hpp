#pragma once

#include <cstddef>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include "coro/task.hpp"
#include "io/buffered_reader.hpp"
#include "io/reader.hpp"

namespace net::io
{

using readline_result = std::expected<std::string, std::error_code>;

// readline reads a line, up to end_of_line, which isn't included. The end of the stream also ends a line.
coro::task<readline_result> readline(buffered_reader* reader,
                                     std::string_view end_of_line = std::string_view{"\r\n"}) noexcept;

// read_terminated_line reads a line that ends with end_of_line (which isn't included), of at most max_length bytes
// (including end_of_line). Unlike readline, a line that the end of the stream cuts short fails with
// errc::end_of_stream, and a longer line with std::errc::value_too_large (having read max_length bytes of it).
coro::task<readline_result> read_terminated_line(buffered_reader* reader,
                                                 std::size_t      max_length,
                                                 std::string_view end_of_line = std::string_view{"\r\n"}) noexcept;

// read_all reads in until the stream ends (errc::end_of_stream), and returns what it read. It fails with the error that
// stopped it otherwise, or std::errc::value_too_large if there's more than max_size bytes.
coro::task<std::expected<std::string, std::error_code>>
read_all(reader& in, std::size_t max_size = std::numeric_limits<std::size_t>::max());
}
