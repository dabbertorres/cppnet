#include "ip_addr.hpp"

#include <array>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <system_error>

#include "util/string_util.hpp"

namespace net
{

std::optional<ipv4_addr> ipv4_addr::parse(std::string_view str) noexcept
{
    std::array<std::uint8_t, 4> values{};

    auto* it = values.end() - 1;
    for (auto part : util::split_string(str, '.'))
    {
        auto res = std::from_chars(part.begin(), part.end(), *it);
        if (res.ec != std::errc()) return std::nullopt;
        --it;
    }

    return ipv4_addr(values);
}

std::string ipv4_addr::to_string() const
{
    // large enough for largest (text) IPv4 address
    std::array<char, 15> buffer{};

    char* end = buffer.begin();

    // network byte order (big endian)
    for (auto i = data.size() - 1; i > 0; --i)
    {
        auto result = std::to_chars(end, end + 3, data[i]);
        end         = result.ptr;
        *end++      = '.';
    }

    auto result = std::to_chars(end, end + 3, data[0]);
    end         = result.ptr;

    return std::string{buffer.data(), end};
}

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

std::optional<ip_addr> ip_addr::parse(std::string_view str) noexcept
{
    if (str.contains(':'))
        if (auto result = ipv6_addr::parse(str); result) return *result;

    if (str.contains('.'))
        if (auto result = ipv4_addr::parse(str); result) return *result;

    return std::nullopt;
}

}
