#pragma once

#include <cstddef>
#include <cstdint>

#include "exception.hpp"

namespace net
{

class writer
{
public:
    virtual ~writer() = default;
    virtual size_t write(const uint8_t* data, size_t length) noexcept = 0;
    virtual void flush() noexcept = 0;

protected:
    writer() = default;
};

}
