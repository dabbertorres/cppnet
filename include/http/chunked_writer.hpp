#pragma once

#include "coro/task.hpp"
#include "http/headers.hpp"
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

    // finish writes the last chunk, which ends the body, with trailers' fields as its trailer section (RFC 9112,
    // section 7.1.2), if it's given any: but those that can't be trailers (see trailer_allowed()), which are dropped.
    // If a trailer's name or value is invalid (see valid_fields(), e.g. a value with a line break), it fails with
    // std::errc::invalid_argument, having written nothing, so the body can be cut short instead.
    //
    // NOTE: if the writer is buffered (an io::buffered_writer) with room for it all, the last chunk is formatted into
    //       its buffer without a coroutine (as HTTP/1.1's heads are), and written by its next flush.
    coro::task<io::result> finish(const trailer_fields* trailers = nullptr);

    [[nodiscard]] io::handle native_handle() const noexcept override { return parent->native_handle(); }

private:
    io::writer* parent = nullptr;
};

}
