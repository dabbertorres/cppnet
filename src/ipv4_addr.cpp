#include "ipv4_addr.hpp"

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
std::optional<ipv4_addr> ipv4_addr::parse(std::string_view str) noexcept
{
    std::array<std::uint8_t, 4> values{};

    std::size_t parsed = 0;
    for (auto part : util::split_string(str, '.'))
    {
        if (parsed == values.size()) return std::nullopt;

        auto& value     = values[parsed++];
        auto [end, err] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (err != std::errc{} || end != part.data() + part.size() || part.empty()) return std::nullopt;
    }

    if (parsed != values.size()) return std::nullopt;
    return ipv4_addr(values);
}

[[nodiscard]] std::string ipv4_addr::to_string() const
{
    // large enough for largest (text) IPv4 address
    std::array<char, 15> buffer{};

    char* end = buffer.begin();

    for (std::size_t i = 0; i < data.size(); ++i)
    {
        if (i != 0) *end++ = '.';
        end = std::to_chars(end, end + 3, data[i]).ptr;
    }

    return std::string{buffer.data(), end};
}

}
