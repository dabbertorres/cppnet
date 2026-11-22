#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

namespace net::http::detail
{

// span_appender appends to a span, as std::string::append does to a string: the span must be long enough.
struct span_appender
{
    std::span<std::byte> out;
    std::size_t          size = 0;

    span_appender& append(std::string_view str) noexcept
    {
        std::ranges::copy(std::as_bytes(std::span{str}), out.subspan(size).begin());
        size += str.size();
        return *this;
    }
};

}
