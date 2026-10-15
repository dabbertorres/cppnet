#pragma once

#include <cstddef>
#include <span>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"

namespace net::io
{

// limit_writer writes at most limit bytes to another writer: e.g. a body of a declared length. A write of more than
// what's left writes what fits, and fails with std::errc::value_too_large.
class limit_writer : public writer
{
public:
    constexpr limit_writer() = default;

    // out is borrowed, and must outlive this limit_writer.
    constexpr limit_writer(writer* out, std::size_t limit) noexcept
        : out{out}
        , left{limit}
    {}

    coro::task<result> write(std::span<const std::byte> data) override;

    using writer::write;

    // remaining is how many bytes can still be written.
    [[nodiscard]] std::size_t remaining() const noexcept { return left; }

    [[nodiscard]] int native_handle() const noexcept override { return out != nullptr ? out->native_handle() : -1; }

private:
    writer*     out  = nullptr;
    std::size_t left = 0;
};

}
