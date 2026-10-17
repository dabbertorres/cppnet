#pragma once

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"

namespace net::http::http11
{

// chunked_writer writes a body in the chunked transfer coding (RFC 9112, section 7.1): each write is a chunk.
// finish() must be called after the last write, to end the body.
class chunked_writer : public io::writer
{
public:
    explicit constexpr chunked_writer(io::writer* writer)
        : parent{writer}
    {}

    // write writes data as one chunk, all of it (unless it fails). An empty write writes nothing: an empty chunk would
    // end the body.
    coro::task<io::result> write(std::span<const std::byte> data) override;

    using io::writer::write;

    // finish writes the last chunk, which ends the body.
    coro::task<io::result> finish();

    [[nodiscard]] io::handle native_handle() const noexcept override { return parent->native_handle(); }

private:
    io::writer* parent = nullptr;
};

}
