#pragma once

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <type_traits>

namespace net::io
{

// handle is a file descriptor. (Windows, whose IOCP event loop isn't implemented, would need a SOCKET.)
using handle = int;

struct result
{
    std::size_t          count{};
    std::error_condition err{};
};

enum class status_condition : std::uint8_t
{
    closed    = 1,
    timed_out = 2,
    error     = 3,
};

std::error_condition make_error_condition(status_condition /*code*/);

const std::error_category& status_condition_category() noexcept;

}

namespace std
{

template<>
struct is_error_condition_enum<net::io::status_condition> : std::true_type
{};

}
