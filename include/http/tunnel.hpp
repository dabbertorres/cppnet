#pragma once

#include <expected>
#include <optional>
#include <stop_token>
#include <system_error>

#include "coro/task.hpp"
#include "http/headers.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/scheduler.hpp"

// Internal: switching a connection to another protocol (e.g. WebSocket), for what speaks it.
namespace net::http::detail
{

// tunnel is a connection's bytes once it has switched protocols: what the peer sends after the request, and what's
// sent to it after the 101 (Switching Protocols); or an HTTP/2 stream's, once a 2xx has answered its extended CONNECT
// (RFC 8441), its DATA both ways. A server's is its connection's (or stream's), valid while the handler that switched
// it runs: once that returns, the connection (or stream) ends. A client's is its own. One task reads it at a time, and
// one writes it at a time.
class tunnel
{
public:
    // reader is what the peer sends: what it sent along with the request (which it shouldn't, but may) is buffered
    // already.
    [[nodiscard]] virtual io::buffered_reader& reader() noexcept = 0;

    // writer is what's sent to the peer: only once flushed, which nothing does but its user (reads don't), until the
    // connection ends, which sends what's left (unless it was aborted).
    [[nodiscard]] virtual io::buffered_writer& writer() noexcept = 0;

    // set_read_timeout limits how long a read may wait without progress (failing with std::errc::timed_out), or not, if
    // none: there's none to begin with (the request's body's was the server's, which the switch takes off). Writes keep
    // their timeout (the server's write_timeout).
    virtual void set_read_timeout(std::optional<io::clock::duration> timeout) noexcept = 0;

    // set_read_deadline limits how long reads may wait altogether: once it has passed, a read that has to wait fails
    // with std::errc::timed_out, having read nothing (until the deadline's changed), or not, if none (as to begin
    // with). It applies to a read that's waiting already too, and may be set from any thread (e.g. to interrupt a read
    // without canceling the writes, as the stop token would).
    virtual void set_read_deadline(std::optional<io::clock::time_point> deadline) noexcept = 0;

    // set_stop_token makes a read or write that waits end when stop is requested, failing with
    // std::errc::operation_canceled: a canceled read has read nothing, so the next one reads what it would have. A
    // default-constructed token (as to begin with) removes it.
    virtual void set_stop_token(std::stop_token stop) noexcept = 0;

    // closing is stopped once the server is closing (its close()), for the protocol to end gracefully (e.g. a
    // WebSocket's close, going away): nothing else ends the tunnel then.
    [[nodiscard]] virtual std::stop_token closing() const noexcept = 0;

    // get_scheduler is the scheduler the connection's I/O waits on (e.g. for what speaks the protocol to wait on, or
    // for its tasks' pool).
    [[nodiscard]] virtual io::scheduler* get_scheduler() const noexcept = 0;

    // abort ends the tunnel abruptly: reads (a waiting one too) find the end of the stream (an HTTP/2 stream's fail,
    // std::errc::connection_aborted, as it's reset, CANCEL), and what's written but not flushed isn't sent when the
    // connection ends. Any thread may call it.
    virtual void abort() noexcept = 0;

protected:
    tunnel() noexcept                         = default;
    tunnel(const tunnel&) noexcept            = default;
    tunnel& operator=(const tunnel&) noexcept = default;
    tunnel(tunnel&&) noexcept                 = default;
    tunnel& operator=(tunnel&&) noexcept      = default;
    ~tunnel()                                 = default;
};

// switch_protocols switches the connection that rw's request came on to another protocol (e.g. WebSocket, whose
// handshake is the caller's), for its handler: it sends 101 (Switching Protocols) with fields (e.g. Upgrade and
// Connection, replacing the response's own of the same names, so fields mustn't be rw.headers() itself) and the rest
// of the response's own (rw.headers(), e.g. a Set-Cookie), flushes it, and returns the connection's tunnel. The
// handler has the tunnel until it returns: then the connection ends (the server closing it first), with no other
// response (rw.switched(), and rw.sent(), say so).
//
// Over HTTP/2, which has no 101, it opens an extended CONNECT's stream (RFC 8441: server_request::protocol is set)
// instead: it answers 200, with fields and the rest of the response's own (but connection-specific ones, which HTTP/2
// doesn't send), without ending the stream, and returns the stream as the tunnel, whose reads and writes are its DATA,
// through buffers of 4 KiB each way. The stream's read timeout is off (the tunnel's own, set_read_timeout(), replaces
// it), and its write timeout stays (the server's write_timeout, past which the stream's reset). Once the handler
// returns, the server ends the stream (END_STREAM, RFC 8441's orderly close).
//
// It fails, sending nothing (so the handler may still respond), with std::errc::not_supported over HTTP/2 for a
// request that isn't an extended CONNECT, to an HTTP/1.0 client (whose Upgrade a server must ignore: RFC 9110, section
// 7.8), and for a response_writer that isn't a server's; with std::errc::invalid_argument for a field that can't be
// sent (e.g. a line break in a value), or a request whose body hasn't been read to its end (what follows its head isn't
// the new protocol's yet); and over HTTP/2, with the stream's failure if it has failed (e.g. reset). It throws
// std::logic_error if the final response was sent already (as send() does). If sending the 101 fails, it returns why:
// the connection has switched then, and ends once the handler returns.
//
// NOTE: when it fails before it needs a coroutine, it throws when it's called, rather than when it's awaited.
coro::task<std::expected<tunnel*, std::error_code>> switch_protocols(response_writer& rw, fields_view fields);

}
