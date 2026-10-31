#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "coro/task.hpp"
#include "io/io.hpp"

// HTTP/2's settings and limits: what the server's and client's configurations hold, without HTTP/2's internals (its
// framing, session and connections), which the public headers (http/server.hpp, http/client.hpp) don't include. Not for
// applications.
namespace net::http::http2
{

inline constexpr std::uint32_t unlimited = std::numeric_limits<std::uint32_t>::max();

// default_window and max_window are a flow-control window's initial size (a stream's, until SETTINGS change it, and a
// connection's) and the largest it may be (RFC 9113, sections 6.9.1 and 6.9.2).
inline constexpr std::uint32_t default_window = 65'535;
inline constexpr std::int64_t  max_window     = 0x7fff'ffff;

// min_max_frame_size and max_max_frame_size are MAX_FRAME_SIZE's bounds.
inline constexpr std::uint32_t min_max_frame_size = 16'384;
inline constexpr std::uint32_t max_max_frame_size = 16'777'215;

// settings are an endpoint's SETTINGS, starting at their initial values (RFC 9113, section 6.5.2; and RFC 8441's
// ENABLE_CONNECT_PROTOCOL, section 3, and RFC 9218's NO_RFC7540_PRIORITIES, section 2.1, each 0 until it's sent).
struct settings
{
    std::uint32_t header_table_size       = 4'096;
    bool          enable_push             = true;
    bool          enable_connect_protocol = false;
    bool          no_rfc7540_priorities   = false;
    std::uint32_t max_concurrent_streams  = unlimited;
    std::uint32_t initial_window_size     = default_window;
    std::uint32_t max_frame_size          = min_max_frame_size;
    std::uint32_t max_header_list_size    = unlimited;

    bool operator==(const settings&) const = default;
};

// token_bucket allows a burst of events, then rate a second.
struct token_bucket
{
    double                                burst  = 0;
    double                                rate   = 0; // a second
    double                                tokens = burst;
    std::chrono::steady_clock::time_point last{};

    // take takes a token, returning false if there are none.
    bool take(std::chrono::steady_clock::time_point now) noexcept;
};

// session_config configures a session: what it advertises, and the limits that defend it.
struct session_config
{
    // This side's settings, sent first. ENABLE_PUSH is always sent as 0 (this side never pushes, nor accepts pushes),
    // and NO_RFC7540_PRIORITIES as 1 (this side sends none of RFC 7540's priority signals, and a server ignores them:
    // RFC 9218, section 2.1).
    // enable_connect_protocol (off by default) has a server send ENABLE_CONNECT_PROTOCOL as 1, and so take extended
    // CONNECT (RFC 8441: a request with :protocol, e.g. a WebSocket's); a client's is never sent, as it means nothing
    // from a client.
    // initial_window_size must be at least its default (65535), as the peer may send that much before it has seen it.
    // max_header_list_size is HPACK's default list size (hpack::default_max_list_size, which http2_session.hpp checks).
    settings local{
        .max_concurrent_streams = 100,
        .max_header_list_size   = std::uint32_t{64} * 1'024,
    };

    // The most this side's HPACK encoder's dynamic table may hold: it uses the smaller of this and the peer's
    // SETTINGS_HEADER_TABLE_SIZE. 4096 is the setting's initial value, and Go's, nghttp2's and h2's cap: a larger table
    // would cost memory per connection, and a longer search per field, for fields few connections repeat. 0 indexes
    // nothing (the first block then says so).
    std::uint32_t encoder_table_size = 4'096;

    // The connection's receive window, raised from its initial 65535 with a WINDOW_UPDATE: how much the peer may send
    // on all streams before this side consumes some. It's to hold every stream's window (local.initial_window_size
    // times the streams that may be open), or streams read one at a time may wait for each other's data to be read:
    // http::client's is 1 GiB, a server's its streams' (at least 1 MiB).
    std::uint32_t connection_window = std::uint32_t{1} << 20;

    // The most CONTINUATION frames a header block may have (nghttp2's default), or, if it's more, as many as a block
    // of local.max_header_list_size takes in 16 KiB frames (the least MAX_FRAME_SIZE, which the peer may send whatever
    // this side's). Its encoded size is limited by local.max_header_list_size. A block past either ends the connection
    // (ENHANCE_YOUR_CALM), as it would have to be decoded anyway, to keep HPACK's table in step.
    std::size_t max_continuations = 8;

    // The most settings a SETTINGS frame may have (Go's limit).
    std::size_t max_settings = 100;

    // The most control frames this side may have to answer (SETTINGS and PING acknowledgements) before its output is
    // taken: past it, the peer is flooding (nghttp2's default).
    std::size_t max_pending_replies = 1'000;

    // The most streams the peer may reset (Rapid Reset, CVE-2023-44487), and the most protocol errors and suspicious
    // frames it may cause (MadeYouReset, empty frames, frames on closed streams), before it's sent GOAWAY
    // (ENHANCE_YOUR_CALM): nghttp2's defaults.
    token_bucket peer_resets{.burst = 1'000, .rate = 33};
    token_bucket glitches{.burst = 10'000, .rate = 330};
};

}

namespace net::http::detail
{

// min_buffer_size is the least a buffer the configurations size may be (server_config's and client_options'
// *_buffer_size, and the WebSocket's): less only costs more reads and writes, and a reader must hold what's parsed in
// place (e.g. HTTP/2's preface, or a WebSocket frame's header).
inline constexpr std::size_t min_buffer_size = 1'024;

// default_read_size is how much an HTTP/2 connection reads at once, by default (server_config's and client_options'
// http2_read_buffer_size).
inline constexpr std::size_t default_read_size = std::size_t{32} * 1'024;

// valid_window is whether window may be an HTTP/2 stream's receive window (server_config's and client_options'
// http2_stream_window, and a WebSocket's http2_window): at least the initial window, which the peer may send before it
// has seen it, and no more than a window may be.
constexpr bool valid_window(std::uint32_t window) noexcept
{
    return window >= http2::default_window && window <= http2::max_window;
}

// writable_function waits until an HTTP/2 connection's socket can take more (see socket::wait_writable()), as its
// writer (a server's or a client's) does before each round with TCP_NOTSENT_LOWAT set: for tests, which hold a
// connection "unwritable" with it.
using writable_function = std::function<coro::task<io::result>()>;

// http2_options are what an HTTP/2 connection is served with: server_config's.
struct http2_options
{
    // How long the preface may take, and a connection may be idle (with no streams) before it's closed; and how long
    // a stream's body may take to arrive, or a response to be sent, without progress, before the stream's reset. 0 is
    // no limit.
    std::chrono::seconds header_read_timeout{};
    std::chrono::seconds body_read_timeout{};
    std::chrono::seconds write_timeout{};

    http2::session_config session{};

    // Whether each stream's priority (RFC 9218) is taken from what the client signals (the request's Priority field,
    // and PRIORITY_UPDATE frames) and the handler's response (its Priority field): server_config::http2_priorities.
    // Off, every stream is u=3, incremental.
    bool priorities = true;

    // The TCP_NOTSENT_LOWAT set on each connection's socket, after which its writer waits for the socket to be
    // writable before each round: server_config::http2_notsent_lowat. 0 sets none, and the writer doesn't wait.
    std::size_t notsent_lowat = 0;

    // Internal (for tests): what the writer waits with instead of the socket, if set (only while notsent_lowat is set):
    // server_config::http2_writable.
    writable_function writable{};

    // The origins sent in ORIGIN after the server's SETTINGS, on connections over TLS: server_config::http2_origins,
    // normalized (and checked against the certificate). Empty sends none.
    std::vector<std::string> origins{};

    // How much a connection reads at once: server_config::http2_read_buffer_size.
    std::size_t read_size = default_read_size;
};

// http2_client_options are what a client's HTTP/2 connections are made with: client_options'.
struct http2_client_options
{
    std::chrono::milliseconds response_header_timeout{}; // 0: no limit
    std::chrono::milliseconds io_timeout{};              // 0: no limit

    // How long a request expecting 100 (Continue) waits for it before sending its body anyway (see
    // client_options::expect_continue_timeout); 0 sends it at once.
    std::chrono::milliseconds expect_continue_timeout{};

    // How long the connection may be idle (no streams, nor any reserved) before it's closed; 0 closes it as soon as
    // its last stream is done.
    std::chrono::milliseconds idle_timeout = std::chrono::seconds{90};

    http2::session_config session{};

    // The TCP_NOTSENT_LOWAT set on each connection's socket, after which its writer waits for the socket to be
    // writable before each round: client_options::http2_notsent_lowat. 0 sets none, and the writer doesn't wait.
    std::size_t notsent_lowat = 0;

    // Internal (for tests): what the writer waits with instead of the socket, if set (only while notsent_lowat is set):
    // client_options::http2_writable.
    writable_function writable{};

    // How much a connection reads at once (client_options::http2_read_buffer_size), and how much of a request's body
    // is read before it's written to its stream (client_options::write_buffer_size).
    std::size_t read_size        = default_read_size;
    std::size_t body_buffer_size = std::size_t{16} * 1'024;
};

}
