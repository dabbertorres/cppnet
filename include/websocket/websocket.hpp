#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "coro/task.hpp"
#include "http/headers.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"
#include "websocket/error.hpp" // IWYU pragma: export

// WebSockets (RFC 6455): a handler accepts one (accept()), and has it, as a connection, until it returns; a client
// opens one (connect()), and has it until it destroys it. A connection reads and writes messages (text or binary),
// whole or streamed, answers the peer's pings and closes by itself, and ends with the close handshake (close()). How a
// WebSocket ended is an error: see websocket/error.hpp.
// NOTE: declared rather than included (http/client.hpp brings the scheduler, and so spdlog), as nothing else here needs
//       them: e.g. linting this header takes a nearby source's command, which may be a fuzz target's, without spdlog.
namespace net::http
{
class client;
}

namespace net::websocket
{

namespace detail
{
    struct connection_state;
}

// message_type is a message's type: text (UTF-8, which is checked as it's read) or binary.
enum class message_type : std::uint8_t
{
    text   = 1,
    binary = 2,
};

// connection_options are a connection's limits, however it was opened.
struct connection_options
{
    // max_message is the most a message received may be (its payload, over all of its frames), however it's read
    // (streamed too): past it, the connection fails with sent(close_code::message_too_big), telling the peer.
    std::uint64_t max_message = std::uint64_t{1} << 20;

    // close_timeout is how long close() waits for the peer's close, once it has sent its own (and a read does, once
    // the server's close() has made it send one: see connection): then it aborts the connection.
    std::chrono::milliseconds close_timeout = std::chrono::seconds{5};

    // idle_timeout is how long a read may wait with nothing received: once half of it has passed, the read pings the
    // peer (whose pong, or anything else it sends, starts the wait over), and if the other half passes too, the peer
    // is taken for gone: the connection fails with std::errc::timed_out, and is aborted. The pings also keep what's in
    // between (e.g. a proxy, such as nginx, whose proxy_read_timeout is 60 s) from closing a WebSocket that's quiet.
    // 0 turns it off. Reads do the timing, with no task or timer of their own: so it applies only while the
    // application reads, as it must anyway, for the peer's pings and close to be answered.
    std::chrono::milliseconds idle_timeout = std::chrono::seconds{60};
};

// accept_options are how a server accepts a WebSocket's handshake.
struct accept_options
{
    // subprotocols are the server's (Sec-WebSocket-Protocol), most preferred first: the first that the client offers
    // is chosen, compared exactly (as browsers compare the choice); none, if the client offers none of them.
    std::vector<std::string> subprotocols{};

    // origins allow handshakes from other origins than the request's own. A request with an Origin (as browsers send)
    // is refused unless the origin's host and port are the request's own (its Host's, a port left out being the
    // origin's scheme's default), or match one of these: a host (ignoring case), each of whose labels may be * for any
    // one label (e.g. "*.example.com", which doesn't match "example.com" nor "a.b.example.com"), optionally with a
    // port, or :* for any (e.g. "localhost:3000"); without one, the origin's port must be its scheme's default. A
    // request without an Origin (e.g. a program's, not a browser's) isn't checked.
    //
    // The check is what keeps a page on another site from opening a WebSocket with the user's cookies (cross-site
    // WebSocket hijacking): browsers send cookies with cross-site WebSocket handshakes.
    std::vector<std::string> origins{};

    // insecure_any_origin skips the origin check: any origin is accepted. Only for endpoints that don't authenticate
    // with what browsers send by themselves (cookies, HTTP authentication, TLS client certificates).
    bool insecure_any_origin = false;

    // connection is the connection's limits.
    connection_options connection{};

    // stop ends the WebSocket once stop is requested: whatever waits (a read, a write, close()) fails with
    // std::errc::operation_canceled, as everything does after it, and the connection is aborted (the peer sees it end
    // without a close). A token without a stop state (as by default) means the request's (req.stop), so the server's
    // abort(), and http::timeout on the WebSocket's route (which then bounds the whole WebSocket), end it.
    std::stop_token stop{};
};

// connect_options are how a client opens a WebSocket.
struct connect_options
{
    // headers are sent with the handshake's request (e.g. Authorization, a Cookie), but for the handshake's own, which
    // replace any of theirs (Upgrade, Connection, and Sec-WebSocket-Key, -Version and -Protocol), and
    // Sec-WebSocket-Extensions, which is dropped: no extension is offered.
    http::headers headers{};

    // subprotocols are offered (Sec-WebSocket-Protocol), in order; the server may choose one of them, or none.
    std::vector<std::string> subprotocols{};

    // origin is sent as the request's Origin, if it's set (browsers send their page's; other programs, none).
    std::string origin{};

    // connection is the connection's limits.
    connection_options connection{};

    // stop ends the WebSocket once stop is requested, whenever: connecting and the handshake fail with
    // std::errc::operation_canceled (whatever else went wrong meanwhile), and once it's open, whatever waits (a read, a
    // write, close()) fails so, as everything does after it, and the connection is aborted (the server sees it end
    // without a close).
    std::stop_token stop{};
};

class connection;

// accept accepts req's WebSocket handshake (RFC 6455, section 4.2), for its handler: it checks the request (see
// accept_options), and if it's not a WebSocket handshake, or one that isn't allowed, responds itself (400 Bad Request;
// 403 Forbidden for an origin; 426 Upgrade Required, with Sec-WebSocket-Version: 13, for another version) and returns
// why (websocket::errc). Otherwise it switches the connection to WebSocket, sending 101 (Switching Protocols) with the
// subprotocol chosen and what's in rw.headers() (e.g. a Set-Cookie), and returns the connection, which is the handler's
// until it returns (see connection).
//
// Over HTTP/2 (RFC 8441), the handshake is an extended CONNECT, which the server presents as GET with
// server_request::protocol "websocket" (see http::server_config::http2_websockets), so the route that serves WebSockets
// over HTTP/1.1 serves them over HTTP/2 too: accept() answers it with 200 (the subprotocol, and rw.headers()), and the
// WebSocket is the request's stream, the rest of the HTTP/2 connection going on beside it. It's the same connection
// (its timeouts, stop and the server's close() alike), counted against http2_max_concurrent_streams until its handler
// returns, which ends the stream (END_STREAM: the client's end of the stream is the WebSocket's end of its transport).
//
// It fails with std::errc::not_supported, responding with nothing (as the handler may), for a response_writer that
// isn't the server's connection's, and with std::errc::invalid_argument for a subprotocol or field of the response's
// that can't be sent. If the response was sent already, it throws std::logic_error. A 101 that can't be sent returns
// the transport's error (the connection then ends once the handler returns); an HTTP/2 stream that has failed (e.g.
// reset by the client) returns its failure, having sent nothing.
coro::task<std::expected<connection, std::error_code>>
accept(const http::server_request& req, http::response_writer& rw, const accept_options& options = {});

// connect opens a WebSocket (RFC 6455, section 4.1) to url: ws:// or wss:// (or http:// or https://, the same over
// HTTP/1.1, as browsers allow), with client's configuration: its resolver, connect_timeout and attempt_delay to
// connect, and for wss, its TLS options (the certificate authorities trusted, a client certificate) and
// tls_handshake_timeout. The connection is the WebSocket's own (never the client's to reuse, nor counted in its
// limits), always HTTP/1.1 (over TLS, it offers only http/1.1 with ALPN). The handshake's request has options' fields,
// and its response must come within response_header_timeout of the request's being sent; writing it, and the
// WebSocket's writes, may go without progress for io_timeout. It fails with:
// - std::errc::invalid_argument for a url that isn't valid, and std::errc::protocol_not_supported for another scheme
//   (or wss without TLS);
// - what connecting failed with (e.g. std::errc::timed_out past connect_timeout, a TLS error), or the handshake's
//   writing or reading (e.g. std::errc::timed_out past response_header_timeout, or a response that isn't HTTP);
// - websocket::errc for a response that isn't the server's accepting (RFC 6455 has the client fail the connection):
//   bad_status for a status other than 101 (Switching Protocols), which includes redirects (they aren't followed, as
//   browsers don't); not_websocket without Upgrade: websocket and Connection: upgrade; bad_accept for the wrong
//   Sec-WebSocket-Accept; bad_subprotocol for a subprotocol that wasn't offered; bad_extension for any extension (none
//   is offered);
// - std::errc::operation_canceled once options.stop is requested.
// What the server sent right after its response is the WebSocket's first frames.
//
// NOTE: url and options are copied before it returns, so they needn't outlive the task; client must (but not the
//       connection, which only needs client's scheduler).
coro::task<std::expected<connection, std::error_code>>
connect(const http::client& client, std::string_view url, const connect_options& options = {});

// message_writer writes a message a frame at a time (see connection::begin_message()): each write() sends its data as
// a frame, flushed before it returns, and end_message() ends the message with a final, empty frame. The connection's
// other messages wait until it's ended, but control frames (ping(), close(), and the replies to the peer's) may go
// between its frames. Once the message has ended, its writer fails with std::errc::invalid_argument.
class message_writer final : public io::writer
{
public:
    // write sends data as one of the message's frames (none, for no data). It fails as connection::write_message()
    // does.
    coro::task<io::result> write(std::span<const std::byte> data) override;

    using io::writer::write;

    // end_message ends the message, letting the connection's other messages go. It must be called, also after a write
    // has failed (it then returns the failure).
    coro::task<std::error_code> end_message();

    [[nodiscard]] io::handle native_handle() const noexcept override;

private:
    friend class connection;

    message_writer(detail::connection_state* state, std::uint64_t message) noexcept
        : state{state}
        , message{message}
    {}

    detail::connection_state* state;
    std::uint64_t             message; // which of the connection's messages written a frame at a time
};

// connection is a WebSocket, open (see accept() and connect()).
//
// One task reads at a time (a read while another's in progress fails with std::errc::device_or_resource_busy): it must
// read, for the peer's pings and close to be answered (as they're answered by reads). Any number of tasks may write at
// once: their messages take turns, a whole message at a time, never interleaved.
//
// Its failures are how it ended (see websocket/error.hpp): the peer's close, as its code (e.g. close_code::going_away;
// close_reason() is its reason), from the read that received it; a close this side sent for what the peer did wrong
// (e.g. sent(close_code::protocol_error)); abnormal_closure if the connection ended without a close; or the transport's
// error (e.g. a write's std::errc::timed_out, past the server's write_timeout or the client's io_timeout; and see
// connection_options::idle_timeout, accept_options::stop and connect_options::stop). Once it has failed, every
// operation returns the same failure. Writing after this side's close has been sent fails with std::errc::broken_pipe
// (as io::status_condition::closed matches).
//
// The server's close() (http::server's) ends a WebSocket that's reading: the read in progress (at once if it's waiting,
// or once it has taken what it has buffered, however busy the peer keeps it) or the next read sends a close saying
// going_away (1001), then reads on for the peer's close (dropping the messages not yet read, those buffered already
// too, and those that come before the close), for up to connection_options::close_timeout (then aborting the
// connection, however much the peer is still sending), and returns sent(close_code::going_away), as everything does
// after it. A WebSocket whose own close() is under way finishes that instead. One that isn't reading isn't told until
// it next reads. So the server's close() then join() drain the WebSockets that are reading.
//
// A server's connection is its handler's until the handler returns: it mustn't outlive the handler's call, which ends
// the HTTP connection. A client's is its own, and ends when it's destroyed. Destroying it without a close (sent by
// close(), or as the reply to the peer's) aborts it: the peer sees the TCP connection end without one
// (abnormal_closure, 1006). Tasks reading or writing must have finished by then.
//
// A client's connection, once the close handshake is done (its close() answered, or its read of the server's close
// answered), waits for the server to end the TCP connection, as RFC 6455 (section 7.1.1) has the server end it first,
// for up to close_timeout: so close(), or the read, returns once it has (or the timeout's passed: the connection then
// ends with its destruction), and the client doesn't hold the closed connection's state (TIME_WAIT) instead of the
// server.
class connection
{
public:
    connection(connection&& other) noexcept;
    connection& operator=(connection&& other) noexcept;
    connection(const connection&)            = delete;
    connection& operator=(const connection&) = delete;
    ~connection();

    // read_message replaces message's contents with the next whole message (skipping what's left of one being
    // streamed), returning its type. Nothing's allocated once message's capacity holds it. A text message is checked to
    // be UTF-8 as it's read (sent(close_code::invalid_payload) otherwise), and any message limited to max_message
    // (sent(close_code::message_too_big)); the peer's close ends the read with the close's code. Pings and pongs that
    // come before (or between the message's frames) are handled: a ping is answered with a pong (only the latest's,
    // if pings come faster than pongs can be sent).
    //
    // NOTE: message must outlive the task (see docs/guide.md, Lifetimes): it's read into as the message comes.
    coro::task<std::expected<message_type, std::error_code>> read_message(std::string& message);
    coro::task<std::expected<message_type, std::error_code>> read_message(std::vector<std::byte>& message);

    // next_message waits for the next message to begin (skipping what's left of one being streamed), returning its
    // type: then message() reads it.
    coro::task<std::expected<message_type, std::error_code>> next_message();

    // message is the message that next_message() began, as a reader: its reads return its data (as it comes, a read
    // returning what's there), and the end of the stream (io::errc::end_of_stream) at its end; a read that fails
    // returns the connection's failure. Each read counts as the connection's read.
    [[nodiscard]] io::reader& message() noexcept;

    // write_message sends a message of type, whose payload is data (UTF-8, for text: as the peer checks), as one frame,
    // once it's this message's turn (see connection), and returns once it has gone to the transport. It fails with
    // std::errc::broken_pipe once this side's close has been sent, and with the transport's error if it fails (which
    // fails the connection).
    coro::task<std::error_code> write_message(message_type type, std::span<const std::byte> data);
    coro::task<std::error_code> write_message(message_type type, std::string_view data)
    {
        return write_message(type, std::as_bytes(std::span{data}));
    }

    // begin_message waits for its turn (see connection), then begins a message of type, to be written a frame at a time
    // (e.g. one too large to hold at once) with the writer it returns, and ended with its end_message(): until then,
    // the connection's other messages wait (a whole message written by the same task would wait for ever).
    coro::task<std::expected<message_writer, std::error_code>> begin_message(message_type type);

    // ping sends a ping, whose payload is data (at most 125 bytes: std::errc::invalid_argument otherwise), between
    // messages' frames. The peer's pong is read as the connection's reads come to it.
    coro::task<std::error_code> ping(std::span<const std::byte> data = {});

    // close begins the close handshake (RFC 6455, section 7), with code (one that can be sent: 1000 to 1003, 1007 to
    // 1014, or 3000 to 4999; or no_status_received, for a close without a code) and reason (UTF-8, at most 123 bytes):
    // otherwise it fails with std::errc::invalid_argument, sending nothing. It sends the close (unless this side has
    // already), then waits for the peer's, for up to connection_options::close_timeout: reading the connection itself
    // (dropping messages that come before the close), unless another task is reading, which it waits for. It returns
    // once the peer's close has come (no error), or aborts the connection and returns why it didn't (e.g.
    // std::errc::timed_out). Once the peer has closed and been answered, there's nothing to wait for.
    coro::task<std::error_code> close(close_code code = close_code::normal_closure, std::string_view reason = {});

    // subprotocol is the subprotocol chosen in the handshake (see accept_options::subprotocols); empty for none.
    [[nodiscard]] std::string_view subprotocol() const noexcept;

    // close_reason is the reason the peer's close gave, once a read (or close()) has received it.
    [[nodiscard]] std::string_view close_reason() const noexcept;

private:
    explicit connection(std::unique_ptr<detail::connection_state> state) noexcept;

    friend coro::task<std::expected<connection, std::error_code>>
    accept(const http::server_request& req, http::response_writer& rw, const accept_options& options);

    friend struct detail::connection_state;

    std::unique_ptr<detail::connection_state> state;
};

}
