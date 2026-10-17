#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "io/reader.hpp"
#include "io/writer.hpp"

namespace net::io
{

class buffer
    : public reader
    , public writer
{
public:
    coro::task<result> write(std::span<const std::byte> data) override;
    coro::task<result> read(std::span<std::byte> data) override;

    using reader::read;
    using writer::write;

    [[nodiscard]] handle native_handle() const noexcept override { return -1; }

    [[nodiscard]] std::string to_string() const;

private:
    std::vector<std::byte> content;
    std::size_t            read_at = 0;
};

}
