#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace net
{

class ipv6_addr
{
public:
    static std::optional<ipv6_addr> parse(std::string_view str) noexcept;
    static constexpr ipv6_addr      loopback() noexcept { return ipv6_addr(0, 0, 0, 0, 0, 0, 0, 1); }

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

    // NOLINTNEXTLINE(*-avoid-c-arrays)
    constexpr explicit ipv6_addr(const std::uint8_t (&parts)[16]) noexcept
        : data{parts[0],
               parts[1],
               parts[2],
               parts[3],
               parts[4],
               parts[5],
               parts[6],
               parts[7],
               parts[8],
               parts[9],
               parts[10],
               parts[11],
               parts[12],
               parts[13],
               parts[14],
               parts[15]}
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

    constexpr explicit ipv6_addr(std::uint8_t p0,
                                 std::uint8_t p1,
                                 std::uint8_t p2,
                                 std::uint8_t p3,
                                 std::uint8_t p4,
                                 std::uint8_t p5,
                                 std::uint8_t p6,
                                 std::uint8_t p7,
                                 std::uint8_t p8,
                                 std::uint8_t p9,
                                 std::uint8_t p10,
                                 std::uint8_t p11,
                                 std::uint8_t p12,
                                 std::uint8_t p13,
                                 std::uint8_t p14,
                                 std::uint8_t p15) noexcept
        : data{p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15}
    {}

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] constexpr std::uint64_t subnet_prefix() const noexcept
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

    [[nodiscard]] constexpr std::uint64_t interface_identifier() const noexcept
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

    [[nodiscard]] constexpr bool is_loopback() const noexcept
    {
        return data.back() == 1 && std::all_of(data.begin(), data.end() - 1, [](auto b) { return b == 0; });
    }

    [[nodiscard]] constexpr bool is_unique_local() const noexcept { return (data[0] & 0xfc) == 0b1111'1100; }
    [[nodiscard]] constexpr bool is_link_local() const noexcept
    {
        return data[0] == 0b1111'1110 && data[1] == 0b1000'0000
            && std::all_of(data.begin() + 2, data.end() - 8, [](auto b) { return b == 0; });
    }

    [[nodiscard]] constexpr bool is_multicast() const noexcept { return data[0] == 0b1111'1111; }

    friend constexpr auto operator<=>(const ipv6_addr& lhs, const ipv6_addr& rhs) noexcept;
    friend constexpr bool operator==(const ipv6_addr& lhs, const ipv6_addr& rhs) noexcept = default;

private:
    // NOTE: network-byte-order
    alignas(std::uint64_t) std::array<std::uint8_t, 16> data;
};

constexpr auto operator<=>(const ipv6_addr& lhs, const ipv6_addr& rhs) noexcept
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
