#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "encoding/hex.hpp"
#include "websocket/error.hpp"
#include "websocket/frame.hpp"
#include "websocket/session.hpp"

// Frames on bytes, for the WebSocket codec's and session's tests (and fuzz_websocket): building them, and reading them
// through a session as its owner would, in pieces of any size.
namespace test::websocket
{

namespace ws = net::websocket::detail;

// bytes is text's bytes.
inline std::vector<std::byte> bytes(std::string_view text)
{
    const auto data = std::as_bytes(std::span{text});
    return {data.begin(), data.end()};
}

// unhex is the bytes hex digits (spaces between them ignored) spell.
inline std::vector<std::byte> unhex(std::string_view hex)
{
    std::string digits;
    std::ranges::copy_if(hex, std::back_inserter(digits), [](char c) { return c != ' '; });
    return net::encoding::hex::decode(digits).value_or(std::vector<std::byte>{});
}

// frame encodes a frame: header, its length set to payload's, then payload, masked with the header's key if it's
// masked.
inline std::vector<std::byte> frame(ws::frame_header header, std::span<const std::byte> payload)
{
    header.length = payload.size();

    std::array<std::byte, ws::max_header_size> head{};
    const auto                                 size = ws::encode_header(header, head);

    std::vector<std::byte> out(head.begin(), head.begin() + static_cast<std::ptrdiff_t>(size));
    const auto             start = out.size();
    out.insert(out.end(), payload.begin(), payload.end());
    if (header.masked) ws::apply_mask(std::span{out}.subspan(start), header.mask_key, 0);
    return out;
}

// join is its arguments' bytes, one after another.
template<typename... T>
std::vector<std::byte> join(const T&... parts)
{
    std::vector<std::byte> out;
    (out.insert(out.end(), std::begin(parts), std::end(parts)), ...);
    return out;
}

// event is a frame a session took, as end_frame() reported it: for message_end, the whole message's data; for a
// control frame, its payload (and a close's code and reason).
struct event
{
    ws::received::kind         what = ws::received::kind::data;
    ws::opcode                 type = ws::opcode::text;
    std::vector<std::byte>     data{};
    net::websocket::close_code code = net::websocket::close_code::no_status_received;
    std::string                reason{};

    bool operator==(const event&) const = default;
};

// read_frame is a frame a session took: its header, and payload unmasked.
struct read_frame
{
    ws::frame_header       header;
    std::vector<std::byte> payload;
};

// trace is what reading input through a session did: the frames it took, in order, and how it failed, if it did, with
// the bytes being read when it did (from failed_from to failed_to): read a byte at a time, the byte that failed is the
// last of them (failed_byte()).
struct trace
{
    std::vector<event>      events;
    std::vector<read_frame> frames;
    std::error_code         failure{};
    std::size_t             failed_from = 0;
    std::size_t             failed_to   = 0;
    std::size_t             used        = 0; // what the frames taken came to

    [[nodiscard]] std::size_t failed_byte() const noexcept { return failed_to - 1; }
};

namespace detail
{
    // to_event is the event for a frame that ended so, with its payload, adding a data frame's to message (the message
    // so far).
    inline event to_event(const ws::received&           end,
                          const ws::frame_info&         info,
                          const std::vector<std::byte>& payload,
                          std::vector<std::byte>&       message)
    {
        event e{.what = end.what, .type = info.type};
        switch (end.what)
        {
        case ws::received::kind::data: message.insert(message.end(), payload.begin(), payload.end()); break;
        case ws::received::kind::message_end:
            message.insert(message.end(), payload.begin(), payload.end());
            e.data = std::exchange(message, {});
            break;
        case ws::received::kind::ping:
        case ws::received::kind::pong: e.data = payload; break;
        case ws::received::kind::close:
            e.data   = payload;
            e.code   = end.close.code;
            e.reason = std::string{end.close.reason};
            break;
        case ws::received::kind::dropped:
            if (info.last) message.clear();
            break;
        }
        return e;
    }
}

namespace detail
{
    // unmasked is a frame's payload, as payload() left it: unmasked, but for a dropped frame's, which is unmasked here,
    // for the frame as it was.
    inline std::vector<std::byte>
    unmasked(std::span<const std::byte> payload, const ws::frame_header& header, ws::received::kind what)
    {
        std::vector<std::byte> out(payload.begin(), payload.end());
        if (what == ws::received::kind::dropped && header.masked) ws::apply_mask(out, header.mask_key, 0);
        return out;
    }
}

// read reads input through s as if it came piece bytes at a time: headers decoded from what's come so far, payloads
// passed to payload() as far as they've come. It stops at a failure, or at the end of input (a frame cut short is left
// unread).
inline trace read(ws::session& s, std::span<const std::byte> input, std::size_t piece)
{
    std::vector<std::byte> buffer(input.begin(), input.end());
    const std::span        buf{buffer};

    trace                  out;
    std::vector<std::byte> message;
    std::size_t            pos   = 0;
    std::size_t            avail = std::min(piece, buf.size());

    const auto more = [&]
    {
        if (avail == buf.size()) return false;
        avail = std::min(buf.size(), avail + piece);
        return true;
    };
    const auto failed = [&](std::error_code err, std::size_t from, std::size_t to)
    {
        out.failure     = err;
        out.failed_from = from;
        out.failed_to   = to;
        return out;
    };

    while (pos < buf.size())
    {
        const auto decoded = ws::decode_header(buf.subspan(pos, avail - pos));
        if (!decoded) return failed(decoded.error(), pos, avail);
        if (!*decoded)
        {
            if (!more()) break;
            continue;
        }

        const auto header = (*decoded)->header;
        pos += (*decoded)->size;
        const auto info = s.begin_frame(header);
        if (!info) return failed(info.error(), pos - 1, pos);

        const auto start = pos;
        while (s.remaining() > 0)
        {
            if (pos == avail && !more()) return out;

            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(s.remaining(), avail - pos));
            if (const auto err = s.payload(buf.subspan(pos, n)); err) return failed(err, pos, pos + n);
            pos += n;
        }

        const auto end = s.end_frame();
        if (!end) return failed(end.error(), pos - 1, pos);

        auto payload = detail::unmasked(buf.subspan(start, pos - start), header, end->what);
        out.events.push_back(detail::to_event(*end, *info, payload, message));
        out.frames.push_back({.header = header, .payload = std::move(payload)});
        out.used = pos;
    }
    return out;
}

}
