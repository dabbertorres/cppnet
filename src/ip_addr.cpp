#include "ip_addr.hpp"

#include <expected>
#include <system_error>

namespace net
{

std::expected<ip_addr, std::error_code> ip_addr::parse(std::string_view str) noexcept
{
    if (str.contains(':'))
        if (auto result = ipv6_addr::parse(str); result) return *result;

    if (str.contains('.'))
        if (auto result = ipv4_addr::parse(str); result) return *result;

    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
}

}
