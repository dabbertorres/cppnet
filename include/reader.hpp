#pragma once

#include <cstddef>
#include <cstdint>

#include "exception.hpp"

namespace net
{

class reader
{
public:
    virtual ~reader() = default;
    virtual size_t read(uint8_t* data, size_t length) noexcept = 0;

protected:
    reader() = default;
};

}
