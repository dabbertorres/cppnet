#pragma once

#include <stdexcept>

namespace net
{

class exception : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
    exception();
    exception(int status);
};

}
