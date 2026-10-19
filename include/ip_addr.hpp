#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"

namespace net
{

enum class protocol : std::uint8_t
{
    not_care,
    ipv4,
    ipv6,
};

class ip_addr
{
public:
    // parse parses an address, failing with std::errc::invalid_argument if str isn't one.
    static std::expected<ip_addr, std::error_code> parse(std::string_view str) noexcept;

    constexpr ip_addr() noexcept = default;

    // NOLINTNEXTLINE(*-explicit-constructor): an ip_addr is either, so converts from both.
    constexpr ip_addr(ipv4_addr addr) noexcept
        : addr{addr}
    {}

    // NOLINTNEXTLINE(*-explicit-constructor): an ip_addr is either, so converts from both.
    constexpr ip_addr(ipv6_addr addr) noexcept
        : addr{addr}
    {}

    [[nodiscard]] constexpr bool is_ipv4() const noexcept { return std::holds_alternative<ipv4_addr>(addr); }
    [[nodiscard]] constexpr bool is_ipv6() const noexcept { return std::holds_alternative<ipv6_addr>(addr); }

    [[nodiscard]] const constexpr ipv4_addr& to_ipv4() const { return std::get<ipv4_addr>(addr); }
    [[nodiscard]] const constexpr ipv6_addr& to_ipv6() const { return std::get<ipv6_addr>(addr); }

    // NOTE: not null-terminated
    // NOLINTNEXTLINE(bugprone-exception-escape): the variant can't be valueless.
    [[nodiscard]] constexpr explicit operator const std::uint8_t*() const noexcept
    {
        return std::visit([](const auto& v) { return static_cast<const std::uint8_t*>(v); }, addr);
    }

    [[nodiscard]] std::string to_string() const
    {
        return std::visit([](const auto& v) { return v.to_string(); }, addr);
    }

    friend constexpr std::partial_ordering operator<=>(const ip_addr& lhs, const ip_addr& rhs) noexcept;

    friend constexpr std::partial_ordering operator<=>(const ip_addr& lhs, const ipv6_addr& rhs) noexcept;
    friend constexpr std::partial_ordering operator<=>(const ip_addr& lhs, const ipv4_addr& rhs) noexcept;

    friend constexpr std::partial_ordering operator<=>(const ipv6_addr& lhs, const ip_addr& rhs) noexcept;
    friend constexpr std::partial_ordering operator<=>(const ipv4_addr& lhs, const ip_addr& rhs) noexcept;

    // NOLINTNEXTLINE(bugprone-exception-escape): the variant can't be valueless.
    friend constexpr bool operator==(const ip_addr& lhs, const ip_addr& rhs) noexcept = default;

private:
    std::variant<ipv4_addr, ipv6_addr> addr;
};

// NOLINTNEXTLINE(bugprone-exception-escape): the variant can't be valueless.
constexpr std::partial_ordering operator<=>(const ip_addr& lhs, const ip_addr& rhs) noexcept
{
    if (lhs.is_ipv4() && rhs.is_ipv4()) return lhs.to_ipv4() <=> rhs.to_ipv4();
    if (lhs.is_ipv6() && rhs.is_ipv6()) return lhs.to_ipv6() <=> rhs.to_ipv6();
    return std::partial_ordering::unordered;
}

}
