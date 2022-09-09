#pragma once

#include <cstddef>
#include <cstdint>

#include "io.hpp"

namespace net
{

class reader
{
public:
    reader(const reader&)                     = default;
    reader& operator=(const reader&) noexcept = default;

    reader(reader&&)                     = default;
    reader& operator=(reader&&) noexcept = default;

    virtual ~reader() = default;

    virtual io_result read(uint8_t* data, size_t length) noexcept = 0;

protected:
    reader() = default;
};

}
