#pragma once

#include <cstddef>

#include "io.hpp"

namespace net::io
{

class reader
{
public:
    reader(const reader&)                     = default;
    reader& operator=(const reader&) noexcept = default;

    reader(reader&&) noexcept            = default;
    reader& operator=(reader&&) noexcept = default;

    virtual ~reader() = default;

    virtual result read(std::byte* data, size_t length) = 0;
    result         read(char* data, size_t length) { return read(reinterpret_cast<std::byte*>(data), length); }

protected:
    reader() = default;
};

}
