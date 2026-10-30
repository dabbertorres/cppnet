#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "http/hpack.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
#include "http/priority.hpp"

// HTTP/2's protocol, without I/O: a session is fed what's read from the connection (receive()), told what the
// application sends (submit_headers(), submit_data(), ...), and gives what's to be written (take_output()). It's
// what HTTP/2's server and client are built from, not for applications.
//
// A session isn't thread-safe: its user serializes calls into it.
namespace net::http::http2
{

enum class role : std::uint8_t
{
    client,
    server,
};

// block_kind is what a header block is: a message's head (a request, or a final response), an informational (1xx)
// response, or trailers.
enum class block_kind : std::uint8_t
{
    head,
    informational,
    trailers,
};

// session_events are what receive() reports, as it happens. Fields and data are views valid during the call.
class session_events
{
public:
    session_events()                                 = default;
    session_events(const session_events&)            = default;
    session_events& operator=(const session_events&) = default;
    session_events(session_events&&)                 = default;
    session_events& operator=(session_events&&)      = default;
    virtual ~session_events()                        = default;

    // on_headers is a valid header block: its fields (pseudo-headers first), in order. An extended CONNECT (RFC 8441:
    // CONNECT with :protocol, :scheme, :path and :authority), which a server's session takes only if its
    // session_config has it send ENABLE_CONNECT_PROTOCOL, is reported as any request, :protocol among its
    // pseudo-headers. A CONNECT's stream (plain or extended; a client's, once its response is a 2xx) is a tunnel: its
    // DATA isn't content, so no Content-Length applies to it, and no trailers may follow its head (RFC 9113, section
    // 8.5).
    virtual void
    on_headers(stream_id stream, block_kind kind, std::span<const hpack::field> fields, bool end_stream) = 0;

    // on_header_list_too_large is a request's head (a server's only) whose decoded header list was over this side's
    // MAX_HEADER_LIST_SIZE: the stream is open, without its fields, for the application to answer (e.g. with 431) or
    // reset; what follows on it is its body, and trailers. (A block over the limit is decoded all the same, to keep
    // HPACK in step, and counted as a glitch. Trailers over it, and a client's response heads, reset the stream.)
    virtual void on_header_list_too_large(stream_id stream, bool end_stream) = 0;

    // on_data is a DATA frame's data (without padding), which counts against the stream's window until consume()d.
    virtual void on_data(stream_id stream, std::span<const std::byte> data, bool end_stream) = 0;

    // on_stream_reset is a stream ended before it was done: reset by the peer (with its code), reset by this side for
    // something malformed the peer sent (protocol_error), or not processed (refused_stream: the peer refused it, or it
    // was after a GOAWAY's last stream, so it may be retried).
    virtual void on_stream_reset(stream_id stream, std::error_code err) = 0;

    // on_window_update is the stream's window growing (or the connection's, for stream 0, which is also reported when
    // a change of settings grows every stream's).
    virtual void on_window_update(stream_id stream) = 0;

    // on_settings is the peer's settings having changed.
    virtual void on_settings(const settings& peer) = 0;

    virtual void on_goaway(stream_id last_stream, std::error_code err, std::string_view debug) = 0;

    virtual void on_ping_ack(std::span<const std::byte, 8> opaque) = 0;

    // on_priority_update is a PRIORITY_UPDATE (RFC 9218, section 7.1) for one of the client's streams the application
    // knows (a server's only: a client's session takes one as a connection error): the stream's new priority, a
    // complete set (what the frame leaves out is the default). One that came before the stream opened is reported
    // just after its head, so it overrides the request's Priority field.
    virtual void on_priority_update(stream_id stream, priority p) = 0;

    // on_origin is an ORIGIN frame (RFC 8336; a client's only: a server's session ignores one): the origins the server
    // says it serves on this connection, its entries as sent (each to be parsed as an origin's ASCII serialization, and
    // ignored if it isn't one), in order; none, for a frame without entries (which still says that the connection is
    // only for its own origin, if it's the first). A frame on a stream other than 0, or with any of the flags 0x1 to
    // 0x8 set, is ignored, as RFC 8336 says (its Appendix B); and an entry whose length runs past the frame is, with
    // the rest of the frame (those before it are reported). Whether to take it (RFC 8336 has a client ignore it over
    // cleartext, and from a proxy) is the application's.
    virtual void on_origin(std::span<const std::string_view> origins) = 0;
};

// session_config's default max_header_list_size is HPACK's (see http2_config.hpp, which doesn't include hpack.hpp).
static_assert(session_config{}.local.max_header_list_size == hpack::default_max_list_size);

// session is one end of an HTTP/2 connection.
class session
{
public:
    // A client's session starts its output with the preface and its SETTINGS; a server's with its SETTINGS, and it
    // expects the client's preface first.
    explicit session(role r, const session_config& config = {});

    // receive processes what's been read, as far as it holds whole frames (and the preface), reporting what happens
    // to events, and returns how much it used: the rest is to be passed again with what's read next. It fails with a
    // connection error (in category(): GOAWAY with it has been queued, and the connection is to be closed once the
    // output is written), after which nothing more is received.
    std::expected<std::size_t, std::error_code>
    receive(std::span<const std::byte> in, session_events& events, std::chrono::steady_clock::time_point now);

    // open_stream opens a client's stream, failing with refused_stream if the server's MAX_CONCURRENT_STREAMS are
    // open, or it has sent GOAWAY, or stream ids have run out.
    std::expected<stream_id, std::error_code> open_stream();

    // can_open_stream is whether open_stream() would succeed now, with reserved more streams to be opened first (e.g.
    // by requests that have been promised them).
    [[nodiscard]] bool can_open_stream(std::size_t reserved = 0) const noexcept;

    // submit_headers sends a header block on a stream: a request's (on a stream from open_stream()), a response's
    // (informational, or final), or trailers (with end_stream). It fails if the stream isn't open for sending.
    //
    // A client's request with :protocol (an extended CONNECT, RFC 8441) may be sent only once the server has sent
    // SETTINGS_ENABLE_CONNECT_PROTOCOL = 1 (peer_settings().enable_connect_protocol): before, it fails with
    // std::errc::invalid_argument, sending nothing, and the stream is closed (nothing was sent on it, so it's not
    // reset: a reset of a stream the server has never seen would be a connection error).
    std::error_code submit_headers(stream_id stream, std::span<const hpack::field> fields, bool end_stream);

    // submit_data sends as much of data as the stream's and connection's windows allow (as DATA frames of at most the
    // peer's MAX_FRAME_SIZE), returning how much it sent: on_window_update() says when to try the rest. end_stream
    // ends the stream if all of data was sent (so empty data ends it whatever the windows).
    std::expected<std::size_t, std::error_code>
    submit_data(stream_id stream, std::span<const std::byte> data, bool end_stream);

    // submit_priority_update asks the server for a new priority for one of the client's streams (a client's only: a
    // server's fails with std::errc::invalid_argument): a PRIORITY_UPDATE frame (RFC 9218, section 7.1), on stream 0,
    // with to_string(p) as its value (a complete set: what it leaves out is the default), an urgency past 7 sent as 7.
    // It returns whether the stream's response may still be reprioritized: the stream is open, and the server hasn't
    // ended it (RFC 9218 has updates go to streams open or half-closed (local) only); otherwise it sends nothing.
    //
    // Only what changes is sent: a stream keeps what it last said (initially its request's Priority field's, if it had
    // one), and an update that says the same sends nothing. An update made while the last one is still waiting to be
    // taken (take_output()) replaces it in place, or takes it back if it says what was said before it; and one waiting
    // when its stream can no longer take it (the server ends it, or it's reset) is taken back. So a caller updating a
    // stream in a loop costs at most a frame each time the output's taken, never one for each call (which a server
    // counting them, as nghttp2's does, could take as a flood).
    std::expected<bool, std::error_code> submit_priority_update(stream_id stream, priority p);

    // submit_origin sends ORIGIN (RFC 8336; a server's only: a client's fails with std::errc::invalid_argument): the
    // origins (each an origin's ASCII serialization, e.g. "https://example.com", which isn't checked) this server
    // serves on the connection, in as few frames as the peer's MAX_FRAME_SIZE allows (one, if they fit; one without
    // entries, if there are none). It fails with std::errc::invalid_argument, sending nothing, if an origin couldn't
    // fit a frame (more than 65535 bytes, or the peer's MAX_FRAME_SIZE but 2); and after a connection error, with it.
    // ORIGIN is to be sent only over TLS (RFC 8336, section 2.2), which the session doesn't know of: that's the
    // caller's to check.
    std::error_code submit_origin(std::span<const std::string> origins);

    // reset resets a stream (RST_STREAM), unless it's already closed.
    void reset(stream_id stream, error_code code);

    // consume says that the application has processed n bytes of the stream's data, giving the peer back that much of
    // the stream's and connection's windows (with WINDOW_UPDATEs, once enough has built up).
    void consume(stream_id stream, std::size_t n);

    // widen_window makes a stream's window (what the peer may have sent that the application hasn't consumed) window,
    // rather than the initial window, if that's larger: a WINDOW_UPDATE for the difference, which stays, as what's
    // consumed is given back as before. Once per stream; nothing for one that's done receiving (or unknown).
    void widen_window(stream_id stream, std::uint32_t window);

    // goaway sends GOAWAY: last_stream is the last of the peer's streams this side will process (max_stream_id, for the
    // first of a graceful shutdown's two). After it, the peer's new streams are ignored.
    void goaway(stream_id last_stream, error_code code, std::string_view debug = {});

    void ping(std::span<const std::byte, 8> opaque);

    // take_output swaps what's to be written with out (which it clears first), so a writer can reuse its buffer.
    void                      take_output(std::vector<std::byte>& out);
    [[nodiscard]] bool        has_output() const noexcept { return !output.empty(); }
    [[nodiscard]] std::size_t output_size() const noexcept { return output.size(); }

    // has_peer_settings is whether the peer's first SETTINGS have arrived.
    [[nodiscard]] bool has_peer_settings() const noexcept { return settings_received; }

    // send_window is how much may be sent on the stream now: the smaller of its window and the connection's (0 if it
    // can't send); for stream 0, the connection's.
    [[nodiscard]] std::int64_t send_window(stream_id stream) const noexcept;

    [[nodiscard]] const settings& peer_settings() const noexcept { return peer; }

    // last_peer_stream is the highest stream id the peer has opened (for GOAWAY).
    [[nodiscard]] stream_id last_peer_stream() const noexcept { return highest_peer_stream; }

    // open_streams is how many streams are open (in either direction).
    [[nodiscard]] std::size_t open_streams() const noexcept { return streams.size(); }

    // goaway_received is whether the peer has sent GOAWAY; failed whether the session has had a connection error.
    [[nodiscard]] bool goaway_received() const noexcept { return peer_goaway.has_value(); }
    [[nodiscard]] bool failed() const noexcept { return connection_failed; }

private:
    // unsaid is stream_state::said's urgency for a stream that's said nothing of its priority.
    static constexpr std::uint8_t unsaid = 0xff;

    // exchange is what a stream's request makes of what follows its heads.
    enum class exchange : std::uint8_t
    {
        plain,
        head,    // a client's request was HEAD: its response has no body
        connect, // a client's request was CONNECT: a 2xx response makes the stream a tunnel
        tunnel,  // a CONNECT's (a server's, from its request; a client's, from its 2xx): DATA isn't content, and no
                 // trailers follow (RFC 9113, section 8.5)
    };

    struct stream_state
    {
        bool     local_closed  = false; // this side has sent END_STREAM
        bool     remote_closed = false; // the peer has sent END_STREAM
        bool     head_received = false; // the peer's (final) head has arrived
        bool     known         = false; // the application knows of it: it opened it, or had its request
        exchange kind          = exchange::plain;
        bool     no_body       = false; // the response is 204 or 304: it has no body
        // what a client's stream last said of its priority (its request's field, then its PRIORITY_UPDATEs), or
        // nothing (urgency unsaid). NOTE: in what was padding, with the flags.
        priority                     said{.urgency = unsaid};
        std::int64_t                 send_window = 0;
        std::int64_t                 recv_window = 0;
        std::uint64_t                unconsumed  = 0; // consumed, but not yet given back with WINDOW_UPDATE
        std::optional<std::uint64_t> content_length;
        std::uint64_t                received = 0; // of DATA
    };

    // pending_update is a PRIORITY_UPDATE in the output, not yet taken: where its frame starts, and what its stream
    // had said before it (so an update back to that takes it back).
    struct pending_update
    {
        stream_id   stream = 0;
        std::size_t at     = 0;
        priority    before;
    };

    // header_block is a header block arriving in a HEADERS frame and CONTINUATIONs.
    struct header_block
    {
        stream_id              stream         = 0;
        bool                   end_stream     = false;
        bool                   self_dependent = false; // its priority made the stream depend on itself
        std::size_t            continuations  = 0;
        std::vector<std::byte> fragment;
    };

    std::expected<std::size_t, std::error_code>
    receive_frame(std::span<const std::byte> in, session_events& events, std::chrono::steady_clock::time_point now);

    std::error_code on_data(const frame_header&                   h,
                            std::span<const std::byte>            payload,
                            session_events&                       events,
                            std::chrono::steady_clock::time_point now);
    std::error_code on_headers(const frame_header&                   h,
                               std::span<const std::byte>            payload,
                               session_events&                       events,
                               std::chrono::steady_clock::time_point now);
    std::error_code on_continuation(const frame_header&                   h,
                                    std::span<const std::byte>            payload,
                                    session_events&                       events,
                                    std::chrono::steady_clock::time_point now);
    std::error_code on_priority(const frame_header&                   h,
                                std::span<const std::byte>            payload,
                                session_events&                       events,
                                std::chrono::steady_clock::time_point now);
    std::error_code on_rst_stream(const frame_header&                   h,
                                  std::span<const std::byte>            payload,
                                  session_events&                       events,
                                  std::chrono::steady_clock::time_point now);
    std::error_code on_settings(const frame_header& h, std::span<const std::byte> payload, session_events& events);
    std::error_code on_ping(const frame_header& h, std::span<const std::byte> payload, session_events& events);
    std::error_code on_goaway(const frame_header& h, std::span<const std::byte> payload, session_events& events);
    std::error_code on_window_update(const frame_header&                   h,
                                     std::span<const std::byte>            payload,
                                     session_events&                       events,
                                     std::chrono::steady_clock::time_point now);
    std::error_code on_priority_update(const frame_header&                   h,
                                       std::span<const std::byte>            payload,
                                       session_events&                       events,
                                       std::chrono::steady_clock::time_point now);
    void            on_origin(const frame_header& h, std::span<const std::byte> payload, session_events& events) const;

    // take_idle_priority takes the PRIORITY_UPDATE kept for a stream that was idle, once a header block on it has
    // arrived (whatever becomes of the block), and drops those of streams that can no longer open (lower ids than the
    // highest the peer has opened, which are closed).
    std::optional<priority> take_idle_priority(stream_id stream);

    // end_header_block decodes a complete header block, and reports it (or resets its stream, if it's malformed; a
    // request's head whose list is over the limit is reported without its fields, on_header_list_too_large()).
    // decode_block decodes it into block_fields; stream_for_block finds its stream, or opens a peer's, returning
    // nullptr if the block is to be ignored (or the stream was refused).
    std::error_code
    end_header_block(const header_block& block, session_events& events, std::chrono::steady_clock::time_point now);
    std::error_code                               decode_block(std::span<const std::byte> fragment);
    std::expected<stream_state*, std::error_code> stream_for_block(stream_id                             id,
                                                                   std::chrono::steady_clock::time_point now);

    // apply_setting applies one of the peer's settings, from its first SETTINGS frame or a later one.
    std::error_code apply_setting(std::uint16_t id, std::uint32_t value, bool first);

    // validated is what validate() found in a header block.
    struct validated
    {
        block_kind                   kind = block_kind::head;
        std::optional<std::uint64_t> content_length;
        int                          status = 0; // a response's

        // what the stream is after the block (as before it, unless a CONNECT's head opens a tunnel)
        exchange stream_kind = exchange::plain;
    };

    // validate checks the decoded header block (block_fields) by HTTP/2's rules (RFC 9113, section 8), returning
    // nullopt if it's malformed.
    [[nodiscard]] std::optional<validated> validate(const stream_state& s, bool end_stream) const;

    // stream_error resets a stream for something malformed the peer sent, reporting it (if the stream was the
    // application's), and counting it as a glitch.
    std::error_code
    stream_error(stream_id stream, error_code code, session_events* events, std::chrono::steady_clock::time_point now);

    // data_error is stream_error for a DATA frame of length the stream can't take, which is given back to the
    // connection's window.
    std::error_code data_error(stream_id                             stream,
                               std::size_t                           length,
                               error_code                            code,
                               session_events&                       events,
                               std::chrono::steady_clock::time_point now);

    // glitch counts something suspicious the peer did, failing the connection if it's done too many.
    std::error_code glitch(std::chrono::steady_clock::time_point now);

    // connection_error queues GOAWAY with code, and fails the session.
    std::error_code connection_error(error_code code, std::string_view debug = {});

    // write_priority_update puts a PRIORITY_UPDATE for stream with p's value into the output at at (replacing the
    // frame there, of length old bytes, if it's not 0), moving the pending updates after it to match.
    void write_priority_update(stream_id stream, priority p, std::size_t at, std::size_t old);

    // take_back_update takes a stream's pending PRIORITY_UPDATE (if it has one) out of the output: it can't take it
    // now (the server's ended it, or it's gone).
    void take_back_update(stream_id stream);

    // close_local and close_remote end one side of a stream, forgetting the stream once both are.
    void close_local(stream_id stream);
    void close_remote(stream_id stream);
    void forget(stream_id stream);

    // remember_reset keeps a stream this side reset, for a while; was_reset is whether it's one.
    void               remember_reset(stream_id stream);
    [[nodiscard]] bool was_reset(stream_id stream) const noexcept;

    // give_back adds to what's to be given back to the peer's windows (the connection's, and the stream's if it's
    // still receiving), sending WINDOW_UPDATEs once half a window has built up.
    void give_back(stream_id stream, std::size_t n);

    // closed is whether a stream that isn't open has been (rather than being idle).
    [[nodiscard]] bool closed(stream_id stream) const noexcept;
    [[nodiscard]] bool peer_initiated(stream_id stream) const noexcept;

    // ignores is whether the peer's frames on a stream that isn't open are ignored: it's one this side reset recently,
    // or one of the peer's after the last stream of this side's GOAWAY (a server's).
    [[nodiscard]] bool ignores(stream_id stream) const noexcept;

    // count_reply counts a control frame queued in reply, failing the connection if too many are waiting.
    std::error_code count_reply();

    role           side;
    session_config config;
    settings       peer;

    hpack::decoder decoder;
    hpack::encoder encoder;

    std::vector<std::byte> output;
    std::size_t            pending_replies = 0;

    bool preface_received  = false; // the client's preface (a server's)
    bool settings_received = false; // the peer's first SETTINGS
    bool connection_failed = false;

    std::unordered_map<stream_id, stream_state> streams;
    stream_id                                   highest_peer_stream = 0;
    stream_id                                   next_local_stream   = 0;
    std::size_t                                 open_peer_streams   = 0;
    std::size_t                                 open_local_streams  = 0;

    // streams this side has reset recently, whose frames in flight are ignored (RFC 9113, section 5.1).
    std::deque<stream_id> recently_reset;

    // the latest PRIORITY_UPDATE for each of the client's streams that are still idle (a server's): reported after the
    // stream's head (see take_idle_priority()). Each is past highest_peer_stream; at most max_concurrent_streams with
    // the open streams (RFC 9218, section 7.1), so a few, and looked through.
    std::vector<std::pair<stream_id, priority>> idle_priorities;

    // the PRIORITY_UPDATEs in the output (a client's), at most one a stream, until it's taken: so few, and looked
    // through (none, nearly always).
    std::vector<pending_update> pending_updates;

    std::int64_t  connection_send_window = 65'535;
    std::int64_t  connection_recv_window = 65'535;
    std::uint64_t connection_unconsumed  = 0;

    std::optional<header_block> pending_block;

    // a decoded header block: its fields' copies (reused from block to block), and views of them.
    std::vector<std::pair<std::string, std::string>> block_storage;
    std::vector<bool>                                block_never_indexed;
    std::size_t                                      block_used = 0;
    std::vector<hpack::field>                        block_fields;

    // for encoding header blocks, reused.
    std::vector<std::byte> encoded_block;

    std::error_code failure; // the connection error, once there is one

    std::optional<stream_id> local_goaway; // the last stream id this side has said it'll process
    std::optional<stream_id> peer_goaway;
};

}
