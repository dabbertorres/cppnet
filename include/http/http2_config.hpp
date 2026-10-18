#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

// HTTP/2's settings and limits: what the server's and client's configurations hold, without HTTP/2's internals (its
// framing, session and connections, see docs/plans/http2.md), which the public headers (http/server.hpp,
// http/client.hpp) don't include. Not for applications.
namespace net::http::http2
{

inline constexpr std::uint32_t unlimited = std::numeric_limits<std::uint32_t>::max();

// min_max_frame_size and max_max_frame_size are MAX_FRAME_SIZE's bounds.
inline constexpr std::uint32_t min_max_frame_size = 16'384;
inline constexpr std::uint32_t max_max_frame_size = 16'777'215;

// settings are an endpoint's SETTINGS, starting at their initial values (RFC 9113, section 6.5.2).
struct settings
{
    std::uint32_t header_table_size      = 4'096;
    bool          enable_push            = true;
    std::uint32_t max_concurrent_streams = unlimited;
    std::uint32_t initial_window_size    = 65'535;
    std::uint32_t max_frame_size         = min_max_frame_size;
    std::uint32_t max_header_list_size   = unlimited;

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
    // This side's settings, sent first. ENABLE_PUSH is always sent as 0 (this side never pushes, nor accepts pushes).
    // initial_window_size must be at least its default (65535), as the peer may send that much before it has seen it.
    // max_header_list_size is HPACK's default list size (hpack::default_max_list_size, which http2_session.hpp checks).
    settings local{
        .max_concurrent_streams = 100,
        .max_header_list_size   = std::uint32_t{64} * 1'024,
    };

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
};

// http2_client_options are what a client's HTTP/2 connections are made with: client_options'.
struct http2_client_options
{
    std::chrono::milliseconds response_header_timeout{}; // 0: no limit
    std::chrono::milliseconds io_timeout{};              // 0: no limit

    // How long the connection may be idle (no streams, nor any reserved) before it's closed; 0 closes it as soon as
    // its last stream is done.
    std::chrono::milliseconds idle_timeout = std::chrono::seconds{90};

    http2::session_config session{};
};

}
