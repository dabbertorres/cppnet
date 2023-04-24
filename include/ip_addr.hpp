#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace net
{

enum class protocol
{
    not_care,
    ipv4,
    ipv6,
};

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

    [[nodiscard]] constexpr explicit operator std::uint32_t() const
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

    friend constexpr auto operator<=>(const ipv4_addr& lhs, const ipv4_addr& rhs) noexcept = default;

private:
    alignas(std::uint32_t) std::array<std::uint8_t, 4> data;
};

class ipv6_addr
{
public:
    static std::optional<ipv6_addr> parse(std::string_view str) noexcept;
    static constexpr ipv6_addr      loopback() noexcept;

    constexpr ipv6_addr() noexcept = default;

    constexpr explicit ipv6_addr(std::uint64_t subnet, std::uint64_t interface) noexcept
        : data{
            static_cast<std::uint8_t>(subnet >> 56),
            static_cast<std::uint8_t>(subnet >> 48),
            static_cast<std::uint8_t>(subnet >> 40),
            static_cast<std::uint8_t>(subnet >> 32),
            static_cast<std::uint8_t>(subnet >> 24),
            static_cast<std::uint8_t>(subnet >> 16),
            static_cast<std::uint8_t>(subnet >> 8),
            static_cast<std::uint8_t>(subnet >> 0),
            static_cast<std::uint8_t>(interface >> 56),
            static_cast<std::uint8_t>(interface >> 48),
            static_cast<std::uint8_t>(interface >> 40),
            static_cast<std::uint8_t>(interface >> 32),
            static_cast<std::uint8_t>(interface >> 24),
            static_cast<std::uint8_t>(interface >> 16),
            static_cast<std::uint8_t>(interface >> 8),
            static_cast<std::uint8_t>(interface >> 0),
        }
    {}

    constexpr explicit ipv6_addr(const std::array<std::uint16_t, 8>& parts) noexcept
        : data{
            static_cast<std::uint8_t>(parts[0] >> 8),
            static_cast<std::uint8_t>(parts[0] >> 0),
            static_cast<std::uint8_t>(parts[1] >> 8),
            static_cast<std::uint8_t>(parts[1] >> 0),
            static_cast<std::uint8_t>(parts[2] >> 8),
            static_cast<std::uint8_t>(parts[2] >> 0),
            static_cast<std::uint8_t>(parts[3] >> 8),
            static_cast<std::uint8_t>(parts[3] >> 0),
            static_cast<std::uint8_t>(parts[4] >> 8),
            static_cast<std::uint8_t>(parts[4] >> 0),
            static_cast<std::uint8_t>(parts[5] >> 8),
            static_cast<std::uint8_t>(parts[5] >> 0),
            static_cast<std::uint8_t>(parts[6] >> 8),
            static_cast<std::uint8_t>(parts[6] >> 0),
            static_cast<std::uint8_t>(parts[7] >> 8),
            static_cast<std::uint8_t>(parts[7] >> 0),
        }
    {}

    constexpr explicit ipv6_addr(const std::array<std::uint8_t, 16>& parts) noexcept
        : data(parts)
    {}

    constexpr explicit ipv6_addr(std::uint16_t p0,
                                 std::uint16_t p1,
                                 std::uint16_t p2,
                                 std::uint16_t p3,
                                 std::uint16_t p4,
                                 std::uint16_t p5,
                                 std::uint16_t p6,
                                 std::uint16_t p7) noexcept
        : data{
            static_cast<std::uint8_t>(p0 >> 8),
            static_cast<std::uint8_t>(p0 >> 0),
            static_cast<std::uint8_t>(p1 >> 8),
            static_cast<std::uint8_t>(p1 >> 0),
            static_cast<std::uint8_t>(p2 >> 8),
            static_cast<std::uint8_t>(p2 >> 0),
            static_cast<std::uint8_t>(p3 >> 8),
            static_cast<std::uint8_t>(p3 >> 0),
            static_cast<std::uint8_t>(p4 >> 8),
            static_cast<std::uint8_t>(p4 >> 0),
            static_cast<std::uint8_t>(p5 >> 8),
            static_cast<std::uint8_t>(p5 >> 0),
            static_cast<std::uint8_t>(p6 >> 8),
            static_cast<std::uint8_t>(p6 >> 0),
            static_cast<std::uint8_t>(p7 >> 8),
            static_cast<std::uint8_t>(p7 >> 0),
        }
    {}

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] constexpr std::uint64_t subnet_prefix() const
    {
        // clang-format off
            return static_cast<std::uint64_t>(data[0]) << 56
                 | static_cast<std::uint64_t>(data[1]) << 48
                 | static_cast<std::uint64_t>(data[2]) << 40
                 | static_cast<std::uint64_t>(data[3]) << 32
                 | static_cast<std::uint64_t>(data[4]) << 24
                 | static_cast<std::uint64_t>(data[5]) << 16
                 | static_cast<std::uint64_t>(data[6]) <<  8
                 | static_cast<std::uint64_t>(data[7]) <<  0;
        // clang-format on
    }

    [[nodiscard]] constexpr std::uint64_t interface_identifier() const
    {
        // clang-format off
            return static_cast<std::uint64_t>(data[8])  << 56
                 | static_cast<std::uint64_t>(data[9])  << 48
                 | static_cast<std::uint64_t>(data[10]) << 40
                 | static_cast<std::uint64_t>(data[11]) << 32
                 | static_cast<std::uint64_t>(data[12]) << 24
                 | static_cast<std::uint64_t>(data[13]) << 16
                 | static_cast<std::uint64_t>(data[14]) <<  8
                 | static_cast<std::uint64_t>(data[15]) <<  0;
        // clang-format on
    }

    // NOTE: not null-terminated
    [[nodiscard]] explicit constexpr operator const std::uint8_t*() const noexcept { return data.data(); }

    friend constexpr auto operator<=>(const ipv6_addr& lhs, const ipv6_addr& rhs) noexcept = default;

private:
    alignas(std::uint64_t) std::array<std::uint8_t, 16> data;
};

class ip_addr
{
public:
    static std::optional<ip_addr> parse(std::string_view str) noexcept;

    constexpr ip_addr() noexcept
        : addr(ipv4_addr())
    {}

    constexpr ip_addr(ipv4_addr addr) noexcept
        : addr(addr)
    {}

    constexpr ip_addr(ipv6_addr addr) noexcept
        : addr(addr)
    {}

    [[nodiscard]] constexpr bool is_ipv4() const noexcept { return std::holds_alternative<ipv4_addr>(addr); }
    [[nodiscard]] constexpr bool is_ipv6() const noexcept { return std::holds_alternative<ipv6_addr>(addr); }

    [[nodiscard]] const constexpr ipv4_addr& to_ipv4() const { return std::get<ipv4_addr>(addr); }
    [[nodiscard]] const constexpr ipv6_addr& to_ipv6() const { return std::get<ipv6_addr>(addr); }

    // NOTE: not null-terminated
    [[nodiscard]] constexpr explicit operator const std::uint8_t*() const noexcept
    {
        return std::visit([](const auto& v) { return static_cast<const std::uint8_t*>(v); }, addr);
    }

    [[nodiscard]] std::string to_string() const
    {
        return std::visit([](const auto& v) { return v.to_string(); }, addr);
    }

    friend constexpr auto operator<=>(const ip_addr& lhs, const ip_addr& rhs) noexcept = default;

private:
    std::variant<ipv4_addr, ipv6_addr> addr;
};

}
