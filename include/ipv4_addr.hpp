#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace net
{

class ipv4_addr
{
public:
    static std::optional<ipv4_addr> parse(std::string_view str) noexcept;
    static constexpr ipv4_addr      loopback() noexcept { return ipv4_addr{127, 0, 0, 1}; }

    constexpr ipv4_addr() noexcept = default;

    constexpr explicit ipv4_addr(std::uint32_t addr) noexcept
        : data{
            static_cast<std::uint8_t>(addr >> 24),
            static_cast<std::uint8_t>(addr >> 16),
            static_cast<std::uint8_t>(addr >> 8),
            static_cast<std::uint8_t>(addr >> 0),
        }
    {}

    constexpr explicit ipv4_addr(const std::array<std::uint8_t, 4>& parts) noexcept
        : data(parts)
    {}

    constexpr explicit ipv4_addr(std::uint8_t b0, std::uint8_t b1, std::uint8_t b2, std::uint8_t b3) noexcept
        : data{b0, b1, b2, b3}
    {}

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] constexpr explicit operator std::uint32_t() const noexcept
    {
        // clang-format off
        return static_cast<std::uint32_t>(data[0]) << 24
             | static_cast<std::uint32_t>(data[1]) << 16
             | static_cast<std::uint32_t>(data[2]) <<  8
             | static_cast<std::uint32_t>(data[3]) <<  0;
        // clang-format on
    }

    // NOTE: not null-terminated
    [[nodiscard]] constexpr explicit operator const std::uint8_t*() const noexcept { return data.data(); }

    [[nodiscard]] constexpr bool is_loopback() const noexcept { return data[0] == 127; }

    [[nodiscard]] constexpr bool is_private_network() const noexcept
    {
        // clang-format off
        return data[0] == 10
            || (data[0] == 172 && (data[1] & 0xf0) == 0x10)
            || (data[0] == 192 && data[2] == 168);
        // clang-format on
    }

    friend constexpr auto operator<=>(const ipv4_addr& lhs, const ipv4_addr& rhs) noexcept;
    friend constexpr bool operator==(const ipv4_addr& lhs, const ipv4_addr& rhs) noexcept = default;

private:
    // NOTE: network-byte-order
    alignas(std::uint32_t) std::array<std::uint8_t, 4> data;
};

constexpr auto operator<=>(const ipv4_addr& lhs, const ipv4_addr& rhs) noexcept
{
    for (auto i = 0U; i < lhs.data.size(); ++i)
    {
        auto cmp = lhs.data[i] <=> rhs.data[i];
        if (std::is_lt(cmp)) return std::strong_ordering::less;
        if (std::is_gt(cmp)) return std::strong_ordering::greater;
    }

    return std::strong_ordering::equivalent;
}

}
