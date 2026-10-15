#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::http::http11
{

// chunked_reader reads a body in the chunked transfer coding (RFC 9112, section 7.1), up to its end: nothing after
// it is read. Chunk extensions and trailers are skipped.
//
// A malformed body fails with std::errc::illegal_byte_sequence, one that ends early with std::errc::connection_aborted,
// and a chunk size line or trailer section that's too long with std::errc::value_too_large.
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

    [[nodiscard]] int native_handle() const noexcept override { return parent->native_handle(); }

private:
    coro::task<std::error_condition> read_chunk_size();
    coro::task<std::error_condition> read_trailers();
    coro::task<std::error_condition> read_crlf();

    std::unique_ptr<io::buffered_reader> owned           = nullptr;
    io::buffered_reader*                 parent          = nullptr;
    std::size_t                          chunk_remaining = 0;
    bool                                 after_chunk     = false; // a chunk was read, but not the CRLF after it
    bool                                 done            = false; // the last chunk and trailers have been read
};

}
