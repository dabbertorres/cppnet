#pragma once

#include <cstddef>
#include <system_error>
#include <type_traits>

namespace net::io
{

// TODO: Linux vs OSX vs Windows, etc
using io_handle = int;

struct result
{
    std::size_t          count{};
    std::error_condition err{};
};

enum class status_condition
{
    closed    = 1,
    timed_out = 2,
    error     = 3,
    // TODO?
};

std::error_condition make_error_condition(status_condition);

const std::error_category& status_condition_category() noexcept;

}

namespace std
{

template<>
struct is_error_condition_enum<net::io::status_condition> : std::true_type
{};

}
