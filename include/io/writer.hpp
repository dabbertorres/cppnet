#pragma once

#include <cstddef>
#include <cstdint>

#include "io.hpp"

namespace net::io
{

template<typename D>
class writer
{
public:
    writer(const writer&)                     = default;
    writer& operator=(const writer&) noexcept = default;

    writer(writer&&) noexcept            = default;
    writer& operator=(writer&&) noexcept = default;

    virtual ~writer() = default;

    virtual result write(const D* data, size_t length) = 0;

protected:
    writer() = default;
};

}