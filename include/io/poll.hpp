#pragma once

#include <cstdint>

#include "coro/task.hpp"
#include "io/io.hpp"

namespace net::io
{

enum class poll_op : std::uint8_t
{
    read       = 1 << 0,
    write      = 1 << 1,
    read_write = read | write,
};

poll_op operator&(poll_op lhs, poll_op rhs) noexcept;

poll_op operator|(poll_op lhs, poll_op rhs) noexcept;

bool is_readable(poll_op op) noexcept;
bool is_writable(poll_op op) noexcept;

enum class poll_status
{
    event,
    timeout,
    error,
    closed,
};

using promise = coro::detail::promise<io::result>;

using namespace std::chrono_literals;

}
