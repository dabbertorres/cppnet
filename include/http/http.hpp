#pragma once

#include <compare>
#include <cstdint>

namespace net::http
{

struct protocol_version
{
    std::uint32_t major;
    std::uint32_t minor;

    friend bool operator==(protocol_version lhs, protocol_version rhs) noexcept = default;
};

constexpr auto operator<=>(protocol_version lhs, protocol_version rhs) noexcept
{
    auto diff = lhs.major <=> rhs.major;
    if (diff != std::strong_ordering::equal) return diff;
    return lhs.minor <=> rhs.minor;
}

}
