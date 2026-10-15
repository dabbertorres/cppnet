#include "ipv6_addr.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "util/string_util.hpp"

namespace net
{

// NOLINTNEXTLINE(bugprone-exception-escape): split_string's substr positions are in range.
std::optional<ipv6_addr> ipv6_addr::parse(std::string_view str) noexcept
{
    // NOTE: currently limited to parsing only fully-specified addresses,
    // rather than shortened representations.

    std::array<std::uint16_t, 8> values{};

    // the parts are stored last first.
    std::size_t parsed = 0;
    for (auto part : util::split_string(str, ':'))
    {
        if (parsed == values.size()) return std::nullopt;

        auto& value     = values[values.size() - 1 - parsed++];
        auto [end, err] = std::from_chars(part.data(), part.data() + part.size(), value, 16);
        if (err != std::errc{} || end != part.data() + part.size() || part.empty()) return std::nullopt;
    }

    if (parsed != values.size()) return std::nullopt;

    return ipv6_addr(values);
}

[[nodiscard]] std::string ipv6_addr::to_string() const
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
