#include "ip_addr.hpp"

#include <array>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <system_error>

namespace net
{

std::optional<ip_addr> ip_addr::parse(std::string_view str) noexcept
{
    if (str.find(':') != std::string_view::npos)
        if (auto result = ipv6_addr::parse(str); result) return *result;

    if (str.find('.') != std::string_view::npos)
        if (auto result = ipv4_addr::parse(str); result) return *result;

    return std::nullopt;
}

}
