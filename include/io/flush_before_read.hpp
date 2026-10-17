#pragma once

#include <cstddef>
#include <span>

#include "coro/task.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::io
{

// flush_before_read reads from a connection, first sending what's been written to it (through writer) so far. So, a
// server holds its responses until it has answered the requests it has already read: a batch of pipelined requests is
// answered with one write, rather than one each, and a request that isn't pipelined is answered as the server waits
// for the next.
//
// It's done on reading, as that's when the server would wait for the client, which may itself be waiting for those
// responses (as a client that doesn't pipeline its requests does): both would then wait forever.
//
// NOTE: so a response to a pipelined request also waits for the requests read along with it to be handled.
class flush_before_read final : public reader
{
public:
    // conn and writer are borrowed, and must outlive the flush_before_read.
    flush_before_read(reader* conn, buffered_writer* writer) noexcept
        : conn{conn}
        , writer{writer}
    {}

    // read fails with errc::end_of_stream if what was written can't be sent: the other end can't be answered any more,
    // which ends the connection as its closing would.
    coro::task<result> read(std::span<std::byte> data) override;

    using reader::read;

    [[nodiscard]] handle native_handle() const noexcept override { return conn->native_handle(); }

private:
    // flush_and_read finishes read(), once flushing (which needed a coroutine) does.
    coro::task<result> flush_and_read(coro::task<result> flushing, std::span<std::byte> data);

    static result closed() noexcept;

    reader*          conn;
    buffered_writer* writer;
};

}
