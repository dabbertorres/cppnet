#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

#include "coro/task.hpp"
#include "io/buffered_reader.hpp"
#include "util/result.hpp"

namespace net::io
{

using namespace std::string_view_literals;

using readline_result = util::result<std::string, std::error_condition>;

// readline reads a line, up to end_of_line, which isn't included. The end of the stream also ends a line.
coro::task<readline_result> readline(buffered_reader* reader, std::string_view end_of_line = "\r\n"sv) noexcept;

// read_terminated_line reads a line that ends with end_of_line (which isn't included), of at most max_length bytes
// (including end_of_line). Unlike readline, a line that the end of the stream cuts short fails with
// status_condition::closed, and a longer line with std::errc::value_too_large (having read max_length bytes of it).
coro::task<readline_result>
read_terminated_line(buffered_reader* reader, std::size_t max_length, std::string_view end_of_line = "\r\n"sv) noexcept;

}
