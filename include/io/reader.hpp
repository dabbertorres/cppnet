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

    virtual result read(std::byte* data, std::size_t length) = 0;
    result         read(char* data, std::size_t length) { return read(reinterpret_cast<std::byte*>(data), length); }

    [[nodiscard]] virtual int native_handle() const noexcept = 0;

protected:
    reader() = default;
};

}
