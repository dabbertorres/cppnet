#include "http/http.hpp"

#include <charconv>

namespace net::http
{

status parse_status(std::string_view str) noexcept
{
    using enum status;

    uint32_t    v;
    const char* begin = reinterpret_cast<const char*>(str.data());
    auto [_, err]     = std::from_chars(begin, begin + str.size(), v);

    if (static_cast<int>(err) != 0) return static_cast<status>(0);
    return static_cast<status>(v);
}

}
