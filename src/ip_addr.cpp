#include "ip_addr.hpp"

#include <optional>

namespace net
{

std::optional<ip_addr> ip_addr::parse(std::string_view str) noexcept
{
    if (str.contains(':'))
        if (auto result = ipv6_addr::parse(str); result) return *result;

    if (str.contains('.'))
        if (auto result = ipv4_addr::parse(str); result) return *result;

    return std::nullopt;
}

}
