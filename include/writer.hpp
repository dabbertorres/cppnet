#pragma once

#include <cstddef>
#include <cstdint>

#include "io.hpp"

namespace net
{

class writer
{
public:
    writer(const writer&)                     = default;
    writer& operator=(const writer&) noexcept = default;

    writer(writer&&)                     = default;
    writer& operator=(writer&&) noexcept = default;

    virtual ~writer() = default;

    virtual io_result            write(const uint8_t* data, size_t length) noexcept = 0;
    virtual std::error_condition flush() noexcept                                   = 0;

protected:
    writer() = default;
};

}
