#pragma once

#include <cstddef>
#include <cstdint>

#include "io.hpp"

namespace net
{

class reader
{
public:
    virtual ~reader() = default;

    virtual io_result read(uint8_t* data, size_t length) noexcept = 0;

protected:
    reader() = default;
};

}
