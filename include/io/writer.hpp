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

    virtual result write(const std::byte* data, std::size_t length) = 0;
    result         write(const char* data, std::size_t length)
    {
        return write(reinterpret_cast<const std::byte*>(data), length);
    }
    result write(std::string_view data) { return write(data.data(), data.size()); }

    [[nodiscard]] virtual int native_handle() const noexcept = 0;

protected:
    writer() = default;
};

}
