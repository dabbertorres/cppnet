#include "websocket/session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <system_error>

#include "util/random.hpp"
#include "websocket/error.hpp"
#include "websocket/frame.hpp"

namespace
{

std::error_code invalid() noexcept { return std::make_error_code(std::errc::invalid_argument); }

}

namespace net::websocket::detail
{

std::expected<frame_info, std::error_code> session::check(const frame_header& header) noexcept
{
    const auto protocol_error = sent(close_code::protocol_error);

    // NOTE: no extension is agreed on (permessage-deflate is out of scope), so no RSV bit has a meaning.
    if (header.rsv != 0 || !is_known(header.op)) return std::unexpected(protocol_error);
    if (header.masked != (side == role::server)) return std::unexpected(protocol_error);

    // after the peer's close, it's to send nothing more (RFC 6455, section 5.5.1): its close was its last frame.
    if (closing == close_state::close_received || closing == close_state::closed)
        return std::unexpected(protocol_error);

    // data, and pings and pongs, that come after this side's close are read past, as the peer may not have seen it
    // yet: only its close is waited for.
    const bool dropping = closing == close_state::close_sent;

    if (is_control(header.op))
    {
        if (!header.fin || header.length > max_control_payload) return std::unexpected(protocol_error);
        const bool dropped = dropping && header.op != opcode::close;
        return frame_info{.what = dropped ? frame_info::kind::dropped : frame_info::kind::control, .type = header.op};
    }

    const bool first = header.op != opcode::continuation;
    if (first == in_message) return std::unexpected(protocol_error);

    const auto type = first ? header.op : message;
    if (dropping)
        return frame_info{.what = frame_info::kind::dropped, .type = type, .first = first, .last = header.fin};

    const auto so_far = first ? 0 : message_size;
    if (header.length > max_message - std::min(so_far, max_message))
        return std::unexpected(sent(close_code::message_too_big));

    return frame_info{.what = frame_info::kind::data, .type = type, .first = first, .last = header.fin};
}

std::expected<frame_info, std::error_code> session::begin_frame(const frame_header& header) noexcept
{
    if (failed) return std::unexpected(failed);
    if (in_frame) return std::unexpected(invalid());

    auto info = check(header);
    if (!info) return std::unexpected(fail(info.error()));

    if (!is_control(header.op))
    {
        // a dropped message's frames are still followed, so that its continuations aren't taken for strays.
        if (info->first)
        {
            message      = info->type;
            message_size = 0;
            utf8.reset();
        }
        in_message = !info->last;
        message_size += header.length;
    }

    what     = info->what;
    op       = header.op;
    fin      = header.fin;
    masked   = header.masked;
    mask_key = header.mask_key;
    left     = header.length;
    offset   = 0;
    in_frame = true;
    return info;
}

std::error_code session::payload(std::span<std::byte> bytes) noexcept
{
    if (failed) return failed;
    if (!in_frame || bytes.size() > left) return invalid();

    if (what != frame_info::kind::dropped)
    {
        if (masked) apply_mask(bytes, mask_key, offset);

        if (what == frame_info::kind::control)
            std::ranges::copy(bytes, control.begin() + static_cast<std::ptrdiff_t>(offset));
        else if (message == opcode::text && !utf8.feed(bytes)) return fail(sent(close_code::invalid_payload));
    }

    left -= bytes.size();
    offset += bytes.size();
    return {};
}

std::expected<received, std::error_code> session::end_frame() noexcept
{
    if (failed) return std::unexpected(failed);
    if (!in_frame || left != 0) return std::unexpected(invalid());
    in_frame = false;

    switch (what)
    {
    case frame_info::kind::dropped: return received{.what = received::kind::dropped};

    case frame_info::kind::data:
        if (!fin) return received{.what = received::kind::data};
        if (message == opcode::text && !utf8.complete())
            return std::unexpected(fail(sent(close_code::invalid_payload)));
        return received{.what = received::kind::message_end};

    case frame_info::kind::control: break;
    }

    const std::span<const std::byte> payload{control.data(), offset};
    if (op == opcode::ping) return received{.what = received::kind::ping, .payload = payload};
    if (op == opcode::pong) return received{.what = received::kind::pong, .payload = payload};

    auto close = parse_close(payload);
    if (!close) return std::unexpected(fail(close.error()));

    peer_close = close->code;
    closing    = closing == close_state::open ? close_state::close_received : close_state::closed;
    return received{.what = received::kind::close, .payload = payload, .close = *close};
}

std::expected<frame_header, std::error_code> session::frame_for(opcode type, bool last, std::uint64_t length) noexcept
{
    if (closing == close_state::close_sent || closing == close_state::closed)
        return std::unexpected(std::make_error_code(std::errc::broken_pipe));
    if (failed && type != opcode::close) return std::unexpected(failed);
    if (!is_known(type) || (length >> 63) != 0) return std::unexpected(invalid());

    if (is_control(type))
    {
        if (!last || length > max_control_payload) return std::unexpected(invalid());
        if (type == opcode::close)
            closing = closing == close_state::close_received ? close_state::closed : close_state::close_sent;
    }
    else
    {
        if ((type != opcode::continuation) == sending) return std::unexpected(invalid());
        sending = !last;
    }

    frame_header header{.fin = last, .op = type, .length = length};
    if (side == role::client)
    {
        header.masked = true;
        util::random_bytes(header.mask_key);
    }
    return header;
}

std::size_t session::close_reply(std::span<std::byte, 2> out) const noexcept
{
    if (peer_close == close_code::no_status_received) return 0;

    const auto code = static_cast<std::uint16_t>(peer_close);
    out[0]          = static_cast<std::byte>(code >> 8);
    out[1]          = static_cast<std::byte>(code & 0xff);
    return 2;
}

}
