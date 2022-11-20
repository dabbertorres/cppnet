#pragma once

#include <cstdint>

namespace net::http
{

struct protocol_version
{
    std::uint32_t major;
    std::uint32_t minor;
};

constexpr bool operator==(protocol_version lhs, protocol_version rhs) noexcept
{
    return lhs.major == rhs.major && lhs.minor == rhs.minor;
}

}
