#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "io.hpp"

namespace net
{

template<typename D>
class reader
{
public:
    reader(const reader&)                     = default;
    reader& operator=(const reader&) noexcept = default;

    reader(reader&&) noexcept            = default;
    reader& operator=(reader&&) noexcept = default;

    virtual ~reader() = default;

    virtual io_result read(D* data, size_t length) = 0;

protected:
    reader() = default;
};

}
