#include "ipv4_addr.hpp"

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

}
