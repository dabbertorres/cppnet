#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
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

// WebSockets (RFC 6455): a handler accepts one (accept()), and has it, as a connection, until it returns. A connection
// reads and writes messages (text or binary), whole or streamed, answers the peer's pings and closes by itself, and
// ends with the close handshake (close()). How a WebSocket ended is an error: see websocket/error.hpp. Opening one, as
// a client, isn't supported yet.
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

    // close_timeout is how long close() waits for the peer's close, once it has sent its own: then it aborts the
    // connection.
    std::chrono::milliseconds close_timeout = std::chrono::seconds{5};
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
};

class connection;

// accept accepts req's WebSocket handshake (RFC 6455, section 4.2), for its handler: it checks the request (see
// accept_options), and if it's not a WebSocket handshake, or one that isn't allowed, responds itself (400 Bad Request;
// 403 Forbidden for an origin; 426 Upgrade Required, with Sec-WebSocket-Version: 13, for another version) and returns
// why (websocket::errc). Otherwise it switches the connection to WebSocket, sending 101 (Switching Protocols) with the
// subprotocol chosen and what's in rw.headers() (e.g. a Set-Cookie), and returns the connection, which is the handler's
// until it returns (see connection).
//
// It fails with std::errc::not_supported, responding with nothing (as the handler may), for a response_writer that
// isn't the server's connection's, and with std::errc::invalid_argument for a subprotocol or field of the response's
// that can't be sent. If the response was sent already, it throws std::logic_error. A 101 that can't be sent returns
// the transport's error (the connection then ends once the handler returns). Over HTTP/2, a request isn't a handshake
// yet (400).
coro::task<std::expected<connection, std::error_code>>
accept(const http::server_request& req, http::response_writer& rw, const accept_options& options = {});

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

// connection is a WebSocket, open (see accept()).
//
// One task reads at a time (a read while another's in progress fails with std::errc::device_or_resource_busy): it must
// read, for the peer's pings and close to be answered (as they're answered by reads). Any number of tasks may write at
// once: their messages take turns, a whole message at a time, never interleaved.
//
// Its failures are how it ended (see websocket/error.hpp): the peer's close, as its code (e.g. close_code::going_away;
// close_reason() is its reason), from the read that received it; a close this side sent for what the peer did wrong
// (e.g. sent(close_code::protocol_error)); abnormal_closure if the connection ended without a close; or the transport's
// error (e.g. a write's std::errc::timed_out, past the server's write_timeout). Once it has failed, every operation
// returns the same failure. Writing after this side's close has been sent fails with std::errc::broken_pipe (as
// io::status_condition::closed matches).
//
// A server's connection is its handler's until the handler returns: it mustn't outlive the handler's call, which ends
// the HTTP connection. Destroying it without a close (sent by close(), or as the reply to the peer's) aborts it: the
// peer sees the TCP connection end without one (abnormal_closure, 1006). Tasks reading or writing must have finished by
// then.
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

    std::unique_ptr<detail::connection_state> state;
};

}
