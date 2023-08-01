#pragma once

#include <cstddef>
#include <string>
#include <system_error>
#include <type_traits>

namespace net::io
{

struct result
{
    std::size_t          count{};
    std::error_condition err{};
};

enum class status_condition
{
    closed = 1,
};

std::error_condition make_error_condition(status_condition);

}

namespace std
{

template<>
struct is_error_condition_enum<net::io::status_condition> : std::true_type
{};

}
