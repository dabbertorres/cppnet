#pragma once

#include <system_error>

namespace net
{

struct io_result
{
    size_t               count;
    std::error_condition err;
};

}
