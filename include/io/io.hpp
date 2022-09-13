#pragma once

#include <system_error>

namespace net::io
{

struct result
{
    size_t               count{};
    std::error_condition err{};
};

}
