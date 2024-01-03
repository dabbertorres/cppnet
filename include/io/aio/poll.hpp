#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>

#include "coro/promise.hpp"
#include "io/io.hpp"

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

using promise = coro::promise<io::result>;

using namespace std::chrono_literals;

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
struct wait_for
{
    std::coroutine_handle<promise> handle;
    int                            fd;
    poll_op                        op;
    std::chrono::milliseconds      timeout;
};

}
