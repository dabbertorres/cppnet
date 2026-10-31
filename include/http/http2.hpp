#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "http/http2_config.hpp"

// HTTP/2's framing (RFC 9113): what HTTP/2's session is built from, not for applications.
namespace net::http::http2
{

using stream_id = std::uint32_t;

// max_stream_id is the largest stream id (31 bits), as max_window (http2_config.hpp) is the largest window.
inline constexpr std::uint32_t max_stream_id = 0x7fff'ffff;

// preface is what a client sends first (RFC 9113, section 3.4), before its SETTINGS frame.
inline constexpr std::string_view preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// error_code is HTTP/2's error codes (RFC 9113, section 7): what RST_STREAM and GOAWAY carry, as std::error_codes in
// category().
// NOLINTNEXTLINE(performance-enum-size): a 32-bit field on the wire, whose unknown values are kept.
enum class error_code : std::uint32_t
{
    no_error            = 0x0,
    protocol_error      = 0x1,
    internal_error      = 0x2,
    flow_control_error  = 0x3,
    settings_timeout    = 0x4,
    stream_closed       = 0x5,
    frame_size_error    = 0x6,
    refused_stream      = 0x7,
    cancel              = 0x8,
    compression_error   = 0x9,
    connect_error       = 0xa,
    enhance_your_calm   = 0xb,
    inadequate_security = 0xc,
    http_1_1_required   = 0xd,
};

// category is HTTP/2's error codes. A code the RFC doesn't define is kept as it is.
[[nodiscard]] const std::error_category& category() noexcept;

[[nodiscard]] std::error_code make_error_code(error_code err) noexcept;

// wire_code is err's code on the wire: its value if it's in category(), and INTERNAL_ERROR otherwise.
[[nodiscard]] std::uint32_t wire_code(std::error_code err) noexcept;

enum class frame_type : std::uint8_t
{
    data          = 0x0,
    headers       = 0x1,
    priority      = 0x2,
    rst_stream    = 0x3,
    settings      = 0x4,
    push_promise  = 0x5,
    ping          = 0x6,
    goaway        = 0x7,
    window_update = 0x8,
    continuation  = 0x9,

    // RFC 8336's (section 2): a server's origins, which a client may send it requests for on this connection, on
    // stream 0.
    origin = 0xc,

    // RFC 9218's (section 7.1): a client's new priority for a stream, on stream 0.
    priority_update = 0x10,
};

// Frames' flags (each meaningful only on some types).
namespace flags
{
    inline constexpr std::uint8_t end_stream  = 0x01; // DATA, HEADERS
    inline constexpr std::uint8_t ack         = 0x01; // SETTINGS, PING
    inline constexpr std::uint8_t end_headers = 0x04; // HEADERS, PUSH_PROMISE, CONTINUATION
    inline constexpr std::uint8_t padded      = 0x08; // DATA, HEADERS, PUSH_PROMISE
    inline constexpr std::uint8_t priority    = 0x20; // HEADERS
}

// frame_header is a frame's first 9 octets. type is kept as it was sent: unknown types must be ignored.
struct frame_header
{
    std::uint32_t length = 0; // of the payload: 24 bits
    std::uint8_t  type   = 0;
    std::uint8_t  flags  = 0;
    stream_id     stream = 0; // 31 bits (the reserved bit is dropped)

    bool operator==(const frame_header&) const = default;
};

inline constexpr std::size_t frame_header_size = 9;

[[nodiscard]] frame_header decode_frame_header(std::span<const std::byte, frame_header_size> in) noexcept;

// encode_frame_header appends a frame's header to out.
void encode_frame_header(const frame_header& header, std::vector<std::byte>& out);

// setting_id is a SETTINGS parameter's identifier (RFC 9113, section 6.5.2; and RFC 8441 and 9218's).
// NOLINTNEXTLINE(performance-enum-size): a 16-bit field on the wire.
enum class setting_id : std::uint16_t
{
    header_table_size       = 0x1,
    enable_push             = 0x2,
    max_concurrent_streams  = 0x3,
    initial_window_size     = 0x4,
    max_frame_size          = 0x5,
    max_header_list_size    = 0x6,
    enable_connect_protocol = 0x8,
    no_rfc7540_priorities   = 0x9,
};

// Encoding frames: each appends a whole frame (header and payload) to out.

// encode_settings encodes the settings that differ from defaults (an empty SETTINGS if none do).
void encode_settings(const settings& values, const settings& defaults, std::vector<std::byte>& out);
void encode_settings_ack(std::vector<std::byte>& out);
void encode_ping(std::span<const std::byte, 8> opaque, bool ack, std::vector<std::byte>& out);
void encode_goaway(stream_id last_stream, std::uint32_t code, std::string_view debug, std::vector<std::byte>& out);
void encode_rst_stream(stream_id stream, std::uint32_t code, std::vector<std::byte>& out);
void encode_window_update(stream_id stream, std::uint32_t increment, std::vector<std::byte>& out);
void encode_data(stream_id stream, std::span<const std::byte> data, bool end_stream, std::vector<std::byte>& out);

// encode_priority_update encodes a PRIORITY_UPDATE frame (RFC 9218, section 7.1), on stream 0: the prioritized stream,
// and a Priority field's value.
void encode_priority_update(stream_id prioritized, std::string_view value, std::vector<std::byte>& out);

// encode_origin encodes an ORIGIN frame (RFC 8336, section 2.1), on stream 0, without flags: an entry for each origin,
// its length (16 bits) and its ASCII serialization. Each must be at most 65535 bytes, and the frame's payload (2 bytes
// an entry, and the origins) fit the peer's MAX_FRAME_SIZE: session::submit_origin() splits them so.
void encode_origin(std::span<const std::string> origins, std::vector<std::byte>& out);

// encode_headers encodes a header block as a HEADERS frame and as many CONTINUATIONs as it needs to keep each frame's
// payload within max_frame_size.
void encode_headers(stream_id                  stream,
                    std::span<const std::byte> block,
                    bool                       end_stream,
                    std::uint32_t              max_frame_size,
                    std::vector<std::byte>&    out);

}

template<>
struct std::is_error_code_enum<net::http::http2::error_code> : std::true_type
{};
