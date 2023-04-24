#include "ip_addr.hpp"

namespace net
{

static constexpr void write_byte(char*& ptr, std::uint8_t b)
{
    if (b >= 100) *ptr++ = '0' + b / 100;

    if (b >= 10) *ptr++ = '0' + (b % 100) / 10;

    *ptr++ = '0' + b % 10;
}

std::string ipv4_addr::to_string() const
{
    // large enough for largest (text) IPv4 address
    std::array<char, 15> buffer{};

    char* end = buffer.data();

    write_byte(end, data[0]);

    for (auto i = 1U; i < buffer.size(); ++i)
    {
        *end++ = '.';
        write_byte(end, data[i]);
    }

    return std::string{buffer.data(), end};
}

std::string ipv6_addr::to_string() const
{
    // large enough for largest (text) IPv6 address
    std::array<char, 40> buffer{};

    char* end = buffer.data();

    // TODO: a lot more complicated than IPv4 - especially if we
    // want to make the string as short as possible

    return std::string{buffer.data(), end};
}

std::optional<ipv4_addr> ipv4_addr::parse(std::string_view str) noexcept
{
    // TODO
}

std::optional<ipv6_addr> ipv6_addr::parse(std::string_view str) noexcept
{
    // TODO
}

std::optional<ip_addr> ip_addr::parse(std::string_view str) noexcept
{
    if (auto result = ipv4_addr::parse(str); result) return *result;
    if (auto result = ipv6_addr::parse(str); result) return *result;
    return std::nullopt;
}

}
