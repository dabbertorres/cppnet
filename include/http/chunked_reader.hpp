#pragma once

#include <cstddef>
#include <memory>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "http/headers.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::http::http11
{

// chunked_reader reads a body in the chunked transfer coding (RFC 9112, section 7.1), up to its end: nothing after
// it is read. Chunk extensions are skipped; the trailer section's fields are kept, if keep_trailers() says where, or
// checked and dropped.
//
// A malformed body fails with std::errc::illegal_byte_sequence, one that ends early with std::errc::connection_aborted,
// and a chunk size line or trailer section that's too long (over max_trailers bytes, or http11::max_fields field
// lines) with std::errc::value_too_large.
class chunked_reader : public io::reader
{
public:
    // A chunk size line, with its extensions. Nothing needs extensions this long.
    static constexpr std::size_t max_size_line = 4'096;

    // The trailer section, altogether.
    static constexpr std::size_t max_trailers = 8'192;

    explicit constexpr chunked_reader(std::unique_ptr<io::buffered_reader>&& reader)
        : owned{std::move(reader)}
        , parent{owned.get()}
    {}

    // reader is borrowed, and must outlive this chunked_reader.
    explicit constexpr chunked_reader(io::buffered_reader* reader)
        : parent{reader}
    {}

    // read reads from at most one chunk, so it doesn't wait for the next chunk when it has data to return.
    coro::task<io::result> read(std::span<std::byte> data) override;

    using io::reader::read;

    [[nodiscard]] io::handle native_handle() const noexcept override { return parent->native_handle(); }

    // call_at_end sets what's called once the body has been read to its end (its last chunk and trailers), by the read
    // that finds it (before it returns).
    void call_at_end(io::end_callback callback) noexcept { at_end = callback; }

    // keep_trailers sets where the trailer section's fields go (or, if trailers is null, nowhere: they're checked and
    // dropped). Once the read that finds the body's end has read them, *trailers holds them (what it held before is
    // cleared then), before call_at_end()'s callback is called: but those that can't be trailers (see
    // trailer_allowed()), which are dropped. A trailer section that fails leaves it empty. trailers must outlive the
    // reads.
    void keep_trailers(headers* trailers) noexcept { kept = trailers; }

    // call_at_start sets what's awaited before the first read into a non-empty span reads anything (see
    // io::start_callback).
    void call_at_start(io::start_callback callback) noexcept { at_start = callback; }

private:
    coro::task<std::error_code> read_chunk_size();
    coro::task<std::error_code> read_trailers();
    coro::task<std::error_code> read_crlf();

    std::unique_ptr<io::buffered_reader> owned           = nullptr;
    io::buffered_reader*                 parent          = nullptr;
    std::size_t                          chunk_remaining = 0;
    bool                                 after_chunk     = false;   // a chunk was read, but not the CRLF after it
    bool                                 done            = false;   // the last chunk and trailers have been read
    headers*                             kept            = nullptr; // where trailers go, if anywhere
    io::end_callback                     at_end;
    io::start_callback                   at_start;
};

}
