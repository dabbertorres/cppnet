#include "websocket/frame.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include "util/utf8.hpp"
#include "websocket/error.hpp"

namespace
{

using net::websocket::close_code;

std::uint8_t byte(std::span<const std::byte> in, std::size_t at) noexcept
{
    return std::to_integer<std::uint8_t>(in[at]);
}

// big_endian reads size bytes at in's start as a big-endian number.
std::uint64_t big_endian(std::span<const std::byte> in, std::size_t size) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) value = (value << 8) | byte(in, i);
    return value;
}

void put_big_endian(std::span<std::byte> out, std::uint64_t value, std::size_t size) noexcept
{
    for (std::size_t i = size; i-- > 0;)
    {
        out[i] = static_cast<std::byte>(value & 0xff);
        value >>= 8;
    }
}

std::error_code protocol_error() noexcept { return net::websocket::sent(close_code::protocol_error); }

bool is_utf8(std::string_view text) noexcept
{
    net::util::utf8_validator utf8;
    return utf8.feed(std::as_bytes(std::span{text})) && utf8.complete();
}

}

namespace net::websocket::detail
{

std::expected<std::optional<decoded_header>, std::error_code> decode_header(std::span<const std::byte> in) noexcept
{
    if (in.size() < 2) return std::nullopt;

    decoded_header out;
    auto&          h = out.header;
    h.fin            = (byte(in, 0) & 0x80) != 0;
    h.rsv            = (byte(in, 0) >> 4) & 0x7;
    h.op             = static_cast<opcode>(byte(in, 0) & 0xf);
    h.masked         = (byte(in, 1) & 0x80) != 0;

    const std::uint8_t length7 = byte(in, 1) & 0x7f;
    std::size_t        size    = 2;
    if (length7 < 126)
    {
        h.length = length7;
    }
    else
    {
        // NOTE: the length is checked as soon as it's all there, so a header fails at the same byte however it's read.
        const std::size_t extended = length7 == 126 ? 2 : 8;
        if (in.size() < size + extended) return std::nullopt;

        h.length = big_endian(in.subspan(size), extended);
        size += extended;
        if (extended == 2 && h.length < 126) return std::unexpected(protocol_error());
        if (extended == 8 && (h.length >> 63) != 0) return std::unexpected(protocol_error());
        if (extended == 8 && h.length <= 0xffff) return std::unexpected(protocol_error());
    }

    if (h.masked)
    {
        if (in.size() < size + h.mask_key.size()) return std::nullopt;
        std::ranges::copy(in.subspan(size, h.mask_key.size()), h.mask_key.begin());
        size += h.mask_key.size();
    }

    out.size = size;
    return out;
}

std::size_t encode_header(const frame_header& header, std::span<std::byte, max_header_size> out) noexcept
{
    out[0] = static_cast<std::byte>((header.fin ? 0x80U : 0U) | ((header.rsv & 0x7U) << 4)
                                    | (static_cast<std::uint8_t>(header.op) & 0xfU));

    const auto  mask = header.masked ? 0x80U : 0U;
    std::size_t size = 2;
    if (header.length < 126)
    {
        out[1] = static_cast<std::byte>(mask | header.length);
    }
    else if (header.length <= 0xffff)
    {
        out[1] = static_cast<std::byte>(mask | 126U);
        put_big_endian(out.subspan(2), header.length, 2);
        size += 2;
    }
    else
    {
        out[1] = static_cast<std::byte>(mask | 127U);
        put_big_endian(out.subspan(2), header.length, 8);
        size += 8;
    }

    if (header.masked)
    {
        std::ranges::copy(header.mask_key, out.begin() + static_cast<std::ptrdiff_t>(size));
        size += header.mask_key.size();
    }
    return size;
}

void apply_mask(std::span<std::byte> data, const std::array<std::byte, 4>& key, std::uint64_t offset) noexcept
{
    // the key, from the byte offset is at, repeated over a word: as a word is a whole number of keys, each word of
    // data from there takes it as it is.
    std::array<std::byte, sizeof(std::uint64_t)> pattern{};
    for (std::size_t i = 0; i < pattern.size(); ++i) pattern[i] = key[(offset + i) & 3];
    std::uint64_t word = 0;
    std::memcpy(&word, pattern.data(), sizeof(word));

    std::size_t i = 0;
    for (; i + sizeof(word) <= data.size(); i += sizeof(word))
    {
        std::uint64_t chunk = 0;
        std::memcpy(&chunk, data.data() + i, sizeof(chunk));
        chunk ^= word;
        std::memcpy(data.data() + i, &chunk, sizeof(chunk));
    }
    for (; i < data.size(); ++i) data[i] ^= pattern[i & 7];
}

std::expected<close_payload, std::error_code> parse_close(std::span<const std::byte> payload) noexcept
{
    if (payload.empty()) return close_payload{};
    if (payload.size() == 1) return std::unexpected(protocol_error());

    const auto code = static_cast<close_code>(big_endian(payload, 2));
    if (!sendable(code)) return std::unexpected(protocol_error());

    const auto rest = payload.subspan(2);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the reason's bytes are its text.
    const std::string_view reason{reinterpret_cast<const char*>(rest.data()), rest.size()};
    if (!is_utf8(reason)) return std::unexpected(protocol_error());

    return close_payload{.code = code, .reason = reason};
}

std::expected<std::size_t, std::error_code>
encode_close(close_code code, std::string_view reason, std::span<std::byte, max_control_payload> out) noexcept
{
    const auto invalid = std::make_error_code(std::errc::invalid_argument);
    if (code == close_code::no_status_received)
        return reason.empty() ? std::expected<std::size_t, std::error_code>{0} : std::unexpected(invalid);
    if (!sendable(code) || reason.size() > max_close_reason || !is_utf8(reason)) return std::unexpected(invalid);

    put_big_endian(out, static_cast<std::uint16_t>(code), 2);
    std::ranges::copy(std::as_bytes(std::span{reason}), out.begin() + 2);
    return reason.size() + 2;
}

}
