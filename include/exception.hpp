#pragma once

#include <stdexcept>
#include <system_error>

namespace net
{

class exception : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
    exception();
    exception(int status);
};

struct io_result
{
    size_t               count;
    std::error_condition err;
};

}
