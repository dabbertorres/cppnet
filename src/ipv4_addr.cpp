#include "ipv4_addr.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdlib>
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

    // the parts are stored last first.
    std::size_t parsed = 0;
    for (auto part : util::split_string(str, '.'))
    {
        if (parsed == values.size()) return std::nullopt;

        auto& value     = values[values.size() - 1 - parsed++];
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

}
