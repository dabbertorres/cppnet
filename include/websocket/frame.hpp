#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include "websocket/error.hpp"

// WebSockets' frames (RFC 6455, section 5): their headers, masking, and close frames' payloads. What WebSockets are
// built from (see docs/plans/websockets.md), not for applications. Nothing here knows the protocol's state: that's
// detail::session's (websocket/session.hpp).
//
// Failures are the error the connection ends with: a close this side sends, with the code (sent(), e.g.
// sent(close_code::protocol_error)), for what's malformed in what the peer sent.
namespace net::websocket::detail
{

// opcode is a frame's type (RFC 6455, section 5.2). Those not named are reserved: a frame may have one (see
// decode_header()), which the session refuses.
enum class opcode : std::uint8_t
{
    continuation = 0x0,
    text         = 0x1,
    binary       = 0x2,
    close        = 0x8,
    ping         = 0x9,
    pong         = 0xa,
};

// is_control is whether op is a control frame's (close, ping, pong, or a reserved one, 0xb to 0xf).
[[nodiscard]] constexpr bool is_control(opcode op) noexcept { return (static_cast<std::uint8_t>(op) & 0x8) != 0; }

// is_known is whether op is one RFC 6455 defines, rather than reserved.
[[nodiscard]] constexpr bool is_known(opcode op) noexcept
{
    switch (op)
    {
    case opcode::continuation:
    case opcode::text:
    case opcode::binary:
    case opcode::close:
    case opcode::ping:
    case opcode::pong: return true;
    }
    return false;
}

// max_header_size is the most a frame's header takes: 2 bytes, an 8-byte extended length and a 4-byte mask key.
inline constexpr std::size_t max_header_size = 14;

// max_control_payload is the most a control frame's payload may be (RFC 6455, section 5.5).
inline constexpr std::size_t max_control_payload = 125;

// max_close_reason is the most a close's reason may be: its payload but the code.
inline constexpr std::size_t max_close_reason = max_control_payload - 2;

// frame_header is a frame's header, before its payload.
struct frame_header
{
    bool                     fin    = true;
    std::uint8_t             rsv    = 0; // RSV1, RSV2 and RSV3, as bits 2, 1 and 0
    opcode                   op     = opcode::text;
    bool                     masked = false;
    std::array<std::byte, 4> mask_key{}; // if masked
    std::uint64_t            length = 0; // the payload's, less than 2^63

    bool operator==(const frame_header&) const = default;
};

// decoded_header is a header, and how many bytes it took.
struct decoded_header
{
    frame_header header;
    std::size_t  size = 0;
};

// decode_header decodes the header at in's start, or gives none if in doesn't hold all of it yet (a header is 2 to 14
// bytes: the second says how many). It fails (with sent(close_code::protocol_error)) for a length the header can't
// have, as soon as in holds the length (even if not yet the mask key): a 64-bit length with its top bit set, or a
// length not in its minimal form (16 bits for less than 126, 64 for less than 65536), which RFC 6455 (section 5.2) says
// it MUST be; refused as the HTTP/1.1 decoder refuses what could be read two ways. Anything else is decoded: reserved
// opcodes, RSV bits, a mask or not, and any length, are the session's to judge.
[[nodiscard]] std::expected<std::optional<decoded_header>, std::error_code>
decode_header(std::span<const std::byte> in) noexcept;

// header_size is how many bytes header's encoding takes.
[[nodiscard]] constexpr std::size_t header_size(const frame_header& header) noexcept
{
    std::size_t size = 2;
    if (header.length > 0xffff) size += 8;
    else if (header.length >= 126) size += 2;
    return header.masked ? size + 4 : size;
}

// encode_header encodes header into out's start (its length in its minimal form; RSV bits and the opcode as they are),
// returning how many bytes it took (header_size()). header.length must be less than 2^63.
std::size_t encode_header(const frame_header& header, std::span<std::byte, max_header_size> out) noexcept;

// apply_mask masks (or unmasks: it's an XOR) data in place with key, as the payload's bytes from offset on (RFC 6455,
// section 5.3), so a payload may be done in pieces, each with its offset in the payload. A word at a time.
void apply_mask(std::span<std::byte> data, const std::array<std::byte, 4>& key, std::uint64_t offset) noexcept;

// close_payload is what a close frame says: its code (no_status_received, if it has none) and reason (UTF-8; empty if
// it has none).
struct close_payload
{
    close_code       code = close_code::no_status_received;
    std::string_view reason{};

    bool operator==(const close_payload&) const = default;
};

// sendable is whether code may be in a close frame (RFC 6455, section 7.4, and IANA's registry): 1000 to 1003, 1007 to
// 1014, and 3000 to 4999. Not those below 1000 (unused), 1004 (reserved), 1005, 1006 and 1015 (which only say what
// happened, never sent), nor 1016 to 2999 (for RFCs, unassigned).
[[nodiscard]] constexpr bool sendable(close_code code) noexcept
{
    const auto c = static_cast<std::uint16_t>(code);
    return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014) || (c >= 3000 && c <= 4999);
}

// parse_close reads a close frame's (unmasked) payload: none, or a code and a reason, whose views are of payload. It
// fails (with sent(close_code::protocol_error)) for a payload of 1 byte, a code that isn't sendable(), or a reason that
// isn't UTF-8.
[[nodiscard]] std::expected<close_payload, std::error_code> parse_close(std::span<const std::byte> payload) noexcept;

// encode_close encodes a close frame's payload into out's start, returning how many bytes it took: none for
// no_status_received (a close without a code, which needs no reason), else the code and reason. It fails with
// std::errc::invalid_argument, writing nothing, for a code that isn't sendable(), a reason without a code, or a reason
// that isn't UTF-8, or is longer than max_close_reason.
[[nodiscard]] std::expected<std::size_t, std::error_code>
encode_close(close_code code, std::string_view reason, std::span<std::byte, max_control_payload> out) noexcept;

}
