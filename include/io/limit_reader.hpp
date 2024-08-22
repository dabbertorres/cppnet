#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include "coro/task.hpp"
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

    coro::task<result> read(std::span<std::byte> data) override;

    using reader::read;

    [[nodiscard]] int native_handle() const noexcept override { return parent->native_handle(); }

private:
    std::unique_ptr<reader> parent   = nullptr;
    std::size_t             limit    = 0;
    std::size_t             progress = 0;
};

}
