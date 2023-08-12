#include "ipv6_addr.hpp"

#include <array>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <system_error>

#include "util/string_util.hpp"

namespace net
{

std::optional<ipv6_addr> ipv6_addr::parse(std::string_view str) noexcept
{
    // NOTE: currently limited to parsing only fully-specified addresses,
    // rather than shortened representations.

    std::array<std::uint16_t, 8> values{};

    auto* it = values.end() - 1;
    for (auto part : util::split_string(str, ':'))
    {
        auto res = std::from_chars(part.begin(), part.end(), *it, 16);
        if (res.ec != std::errc()) return std::nullopt;
        --it;
    }

    return ipv6_addr(values);
}

std::string ipv6_addr::to_string() const
{
    // large enough for largest (text) IPv6 address
    std::array<char, 39> buffer{};

    char* end = buffer.begin();

    // TODO: a lot more complicated than IPv4 - especially if we
    // want to make the string as short as possible
    // For getting it somewhat working, do (almost) the same thing as ipv4_addr

    for (auto i = data.size() - 1; i > 1; i -= 2)
    {
        auto hexit = static_cast<std::uint16_t>(data[i]) | static_cast<std::uint16_t>(data[i - 1]) << 8;

        auto result = std::to_chars(end, end + 4, hexit, 16);
        end         = result.ptr;

        *end++ = ':';
    }

    auto hexit = static_cast<std::uint16_t>(data[1]) | static_cast<std::uint16_t>(data[0]) << 8;

    auto result = std::to_chars(end, end + 4, hexit, 16);
    end         = result.ptr;

    return std::string{buffer.data(), end};
}

}
