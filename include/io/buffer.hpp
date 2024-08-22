#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "coro/task.hpp"
#include "io/io.hpp"
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
    [[nodiscard]] int  native_handle() const noexcept override { return -1; }

    [[nodiscard]] std::string to_string() const;

private:
    std::vector<std::byte> content;
    std::size_t            write_at = 0;
    std::size_t            read_at  = 0;
};

}
