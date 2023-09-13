#pragma once

#include <cstdint>

namespace net::io::aio
{

enum class poll_op : std::uint8_t
{
    read       = 1 << 0,
    write      = 1 << 1,
    read_write = read | write,
};

poll_op operator&(poll_op lhs, poll_op rhs) noexcept;

poll_op operator|(poll_op lhs, poll_op rhs) noexcept;

bool readable(poll_op op) noexcept;
bool writable(poll_op op) noexcept;

enum class poll_status
{
    event,
    timeout,
    error,
    closed,
};

}