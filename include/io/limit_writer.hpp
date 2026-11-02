#pragma once

#include <cstddef>
#include <span>

#include "coro/adapter.hpp"
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

    [[nodiscard]] handle native_handle() const noexcept override { return out != nullptr ? out->native_handle() : -1; }

private:
    // finish counts what a write of amount bytes (of wanted) wrote.
    result finish(result res, std::size_t amount, std::size_t wanted) noexcept;

    // finish_waiting is finish() for the write that waited (finishing's).
    result finish_waiting(result res) noexcept { return finish(res, waiting_amount, waiting_wanted); }

    writer*     out  = nullptr;
    std::size_t left = 0;

    // finishing finishes a write that has to wait (of waiting_amount bytes, of waiting_wanted), without a coroutine.
    std::size_t                                                                waiting_amount = 0;
    std::size_t                                                                waiting_wanted = 0;
    coro::adapter<limit_writer, result, result, &limit_writer::finish_waiting> finishing{this};
};

}
