#pragma once

#include <string>
#include <string_view>
#include <system_error>

#include "io/buffered_reader.hpp"
#include "util/result.hpp"

namespace net::io
{

using namespace std::string_view_literals;

using readline_result = util::result<std::string, std::error_condition>;

readline_result readline(buffered_reader& reader, std::string_view end_of_line = "\r\n"sv) noexcept;

}
