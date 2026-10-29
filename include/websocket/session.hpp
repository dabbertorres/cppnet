#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <system_error>

#include "util/utf8.hpp"
#include "websocket/error.hpp"
#include "websocket/frame.hpp"

// WebSockets' protocol, without I/O: a session judges the frames read from the connection, and gives the headers of
// those to write, by the rules of RFC 6455. It's what a WebSocket's connection is built from, not for applications.
//
// It's pull-shaped, rather than event-shaped as http2::session is, so that a payload is never copied: its owner
// decodes a header (decode_header()) and passes it to begin_frame(), reads the payload into a buffer of its own (or its
// caller's) and hands it over in pieces to payload(), which unmasks and checks it in place, then calls end_frame(),
// which says what the frame was. Sending, frame_for() gives a frame's header, which the owner writes before the payload
// (masked with the header's key, for a client).
//
// A session isn't thread-safe: its owner serializes calls into it.
namespace net::websocket::detail
{

enum class role : std::uint8_t
{
    client,
    server,
};

struct limits
{
    // max_message is the most a message's payload may be, over all of its frames (a frame is bounded by nothing else):
    // past it, the session fails with message_too_big.
    std::uint64_t max_message = std::uint64_t{1} << 20;
};

// frame_info is what begin_frame() says a frame is, and so what to do with its payload.
struct frame_info
{
    enum class kind : std::uint8_t
    {
        data,    // part of a message: its payload is the message's
        control, // a close, ping or pong: its payload is collected by the session (at most 125 bytes)
        dropped, // received after this side's close (data, a ping or a pong): its payload is to be read past
    };

    kind what = kind::data;
    // type is the message's type (text or binary), on each of its frames, for data; otherwise the frame's opcode.
    opcode type = opcode::text;
    // first and last say whether a data frame is its message's first, and last (fin).
    bool first = false;
    bool last  = false;
};

// received is what end_frame() says a frame was.
struct received
{
    enum class kind : std::uint8_t
    {
        data,        // part of a message, not its last
        message_end, // its message's last frame: the message is whole (and, for text, UTF-8)
        ping,        // a ping, to be answered with a pong of its payload
        pong,        // a pong (answering a ping, or unsolicited)
        close,   // the peer's close: its code and reason; to be answered, if this side hasn't sent one (close_reply())
        dropped, // a frame received after this side's close, its payload read past
    };

    kind what = kind::data;
    // payload is a ping's, pong's or close's payload, unmasked: the session's, valid until the next frame.
    std::span<const std::byte> payload{};
    // close is a close's code (no_status_received, for none) and reason (a view of payload).
    close_payload close{};
};

// close_state is how far the close handshake has gone (RFC 6455, section 7).
enum class close_state : std::uint8_t
{
    open,           // neither side has sent a close
    close_sent,     // this side has sent one: only the peer's close is taken; data, pings and pongs are dropped
    close_received, // the peer has sent one: nothing more is taken; this side's close is to be the reply
    closed,         // both have: the connection is to end
};

// session is one end of a WebSocket.
class session
{
public:
    // A server's session refuses frames that aren't masked, and a client's those that are (RFC 6455, section 5.1).
    explicit session(role r, const limits& l = {}) noexcept
        : max_message{l.max_message}
        , side{r}
    {}

    // begin_frame checks a received frame's header against the rules and the session's state, and says what the frame
    // is, the frame then being the current one, whose payload is passed to payload() (all of it, header.length bytes,
    // in any pieces) before end_frame(). It fails, with the close for this side to send (sent()), for:
    // - RSV bits set (no extension is agreed on, which could give them a meaning);
    // - a reserved opcode;
    // - a frame that isn't masked to a server, or is to a client;
    // - a control frame that's fragmented (not fin), or with a payload over 125 bytes;
    // - a continuation with no message started, or a text or binary frame inside one (each message's frames are
    //   contiguous: only control frames come between them);
    // - any frame after the peer's close;
    // all with protocol_error; and a message (its frames so far, and this one's length) over limits::max_message, with
    // message_too_big. A failure is kept: every later call to begin_frame(), payload() or end_frame() returns it.
    [[nodiscard]] std::expected<frame_info, std::error_code> begin_frame(const frame_header& header) noexcept;

    // payload takes bytes of the current frame's payload, after those passed before: it unmasks them in place (a data
    // frame's, which are then the message's; but not a dropped frame's, which are only counted), checks that a text
    // message's are UTF-8 as they come (a character may be split across pieces and frames, but a byte that can't be
    // where it is fails at once, with invalid_payload), and collects a control frame's. More bytes than are left of
    // the frame fail with std::errc::invalid_argument (not kept).
    [[nodiscard]] std::error_code payload(std::span<std::byte> bytes) noexcept;

    // remaining is how many bytes of the current frame's payload are still to be passed to payload().
    [[nodiscard]] std::uint64_t remaining() const noexcept { return left; }

    // end_frame finishes the current frame, once all of its payload has been passed (std::errc::invalid_argument, not
    // kept, otherwise), and says what it was. It fails with invalid_payload for a text message whose last frame ends
    // part way through a character, and with protocol_error for a close whose payload is malformed (see
    // parse_close()). A close moves the close state: from open to close_received, from close_sent to closed.
    [[nodiscard]] std::expected<received, std::error_code> end_frame() noexcept;

    // frame_for gives the header for a frame this side sends: its payload of length bytes, masked with the header's key
    // (a fresh one from util::random_bytes()) if it's a client's. It fails with std::errc::invalid_argument for what
    // RFC 6455 doesn't allow (a reserved opcode; a control frame fragmented, or over 125 bytes; a continuation without
    // a message started, or a text or binary frame while one is; a length of 2^63 or more), and with
    // std::errc::broken_pipe once this side's close has been sent; and after a receiving failure, with that failure,
    // for anything but the close that fails the connection. A close moves the close state: from open to close_sent,
    // from close_received to closed.
    [[nodiscard]] std::expected<frame_header, std::error_code>
    frame_for(opcode type, bool last, std::uint64_t length) noexcept;

    // close_reply encodes the payload of this side's reply to the peer's close into out, returning how many bytes it
    // took: the peer's code (as browsers and gorilla/websocket echo it), or nothing if its close had none.
    [[nodiscard]] std::size_t close_reply(std::span<std::byte, 2> out) const noexcept;

    [[nodiscard]] close_state state() const noexcept { return closing; }

    // failure is what the session failed with, receiving: the close this side is to send (sent()); none if it hasn't.
    [[nodiscard]] std::error_code failure() const noexcept { return failed; }

private:
    std::error_code fail(std::error_code err) noexcept
    {
        failed = err;
        return err;
    }

    std::expected<frame_info, std::error_code> check(const frame_header& header) noexcept;

    std::array<std::byte, max_control_payload> control{}; // the current control frame's payload, as collected

    util::utf8_validator utf8; // the current text message's

    std::uint64_t   max_message;
    std::uint64_t   message_size = 0; // the current message's frames so far (received)
    std::uint64_t   left         = 0; // of the current frame's payload
    std::uint64_t   offset       = 0; // of the current frame's payload passed so far
    std::error_code failed{};

    std::array<std::byte, 4> mask_key{}; // the current frame's, if masked

    role        side;
    close_state closing    = close_state::open;
    close_code  peer_close = close_code::no_status_received; // the code of the peer's close, once received

    // the current frame's
    frame_info::kind what     = frame_info::kind::data;
    opcode           op       = opcode::continuation;
    bool             fin      = false;
    bool             masked   = false;
    bool             in_frame = false;

    bool   in_message = false;        // received: a message's first frame has come, and not its last
    opcode message    = opcode::text; // received: its type
    bool   sending    = false;        // sent: a message's first frame has gone, and not its last
};

}
