#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

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

    constexpr limit_reader(std::unique_ptr<reader>&& reader, std::size_t limit)
        : parent{std::move(reader)}
        , limit{limit}
    {}

    result read(std::span<std::byte> data) override
    {
        if (progress >= limit) return {};

        const std::size_t read_amount = std::min(data.size(), limit - progress);

        auto res = parent->read(data.subspan(0, read_amount));
        progress += res.count;
        return res;
    }

    using reader::read;

    [[nodiscard]] int native_handle() const noexcept override { return parent->native_handle(); }

private:
    std::unique_ptr<reader> parent   = nullptr;
    std::size_t             limit    = 0;
    std::size_t             progress = 0;
};

}
