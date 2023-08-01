#pragma once

#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::io
{

class limit_reader : public reader
{
public:
    constexpr limit_reader()
        : limit_reader(nullptr, 0)
    {}

    constexpr limit_reader(reader* reader, std::size_t limit)
        : parent{reader}
        , limit{limit}
    {}

    result read(std::byte* data, std::size_t length) override
    {
        if (progress >= limit) return {};

        const std::size_t read_amount = std::min(length, limit - progress);

        auto res = parent->read(data, read_amount);
        progress += res.count;
        return res;
    }

    using reader::read;

    [[nodiscard]] int native_handle() const noexcept override { return parent->native_handle(); }

private:
    reader*     parent   = nullptr;
    std::size_t limit    = 0;
    std::size_t progress = 0;
};

}
