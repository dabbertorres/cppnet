#pragma once

#include <cstddef>

#include "io.hpp"

namespace net::io
{

class writer
{
public:
    writer(const writer&)                     = default;
    writer& operator=(const writer&) noexcept = default;

    writer(writer&&) noexcept            = default;
    writer& operator=(writer&&) noexcept = default;

    virtual ~writer() = default;

    virtual result write(const byte* data, size_t length) = 0;
    result         write(const char* data, size_t length) { return write(reinterpret_cast<const byte*>(data), length); }

protected:
    writer() = default;
};

}
