#include "http/http2_session.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
#include "http/priority.hpp"
#include "http/structured_fields.hpp"
#include "util/string_util.hpp"

namespace net::http::http2
{

namespace
{

    using time_point = std::chrono::steady_clock::time_point;

    // The most streams this side remembers resetting, to ignore what the peer had already sent on them.
    constexpr std::size_t max_remembered_resets = 128;

    // Until the server's SETTINGS arrive, a client assumes it may open this many streams (as Go and Chromium do).
    constexpr std::uint32_t assumed_max_concurrent_streams = 100;

    // The most PRIORITY_UPDATEs a server keeps for streams not yet open, whatever its MAX_CONCURRENT_STREAMS (which
    // bounds them first, as RFC 9218 has it, unless it's larger): past it, they're ignored, as glitches.
    constexpr std::size_t max_idle_priorities = 128;

    std::uint32_t read32(std::span<const std::byte> in) noexcept
    {
        std::uint32_t value = 0;
        std::memcpy(&value, in.data(), sizeof(value));
        if constexpr (std::endian::native == std::endian::little) value = std::byteswap(value);
        return value;
    }

    std::uint16_t read16(std::span<const std::byte> in) noexcept
    {
        std::uint16_t value = 0;
        std::memcpy(&value, in.data(), sizeof(value));
        if constexpr (std::endian::native == std::endian::little) value = std::byteswap(value);
        return value;
    }

    // unpad is a payload without its padding (and the padding's length), or nullopt if the padding is as long as the
    // payload (RFC 9113, section 6.1).
    std::optional<std::span<const std::byte>> unpad(std::span<const std::byte> payload, std::uint8_t frame_flags)
    {
        if ((frame_flags & flags::padded) == 0) return payload;
        if (payload.empty()) return std::nullopt;

        const auto padding = std::to_integer<std::size_t>(payload[0]);
        if (padding >= payload.size()) return std::nullopt;
        return payload.subspan(1, payload.size() - 1 - padding);
    }

    // is_token_lower is whether c may be in an HTTP/2 field name: a token character (RFC 9110, section 5.6.2), but not
    // an uppercase letter (RFC 9113, section 8.2.1).
    bool is_name_char(char c) noexcept
    {
        if (c >= 'a' && c <= 'z') return true;
        if (c >= '0' && c <= '9') return true;
        return std::string_view{"!#$%&'*+-.^_`|~"}.contains(c);
    }

    bool valid_name(std::string_view name) noexcept { return !name.empty() && std::ranges::all_of(name, is_name_char); }

    // valid_value is whether a field value has no controls but tab (as HTTP/1.1's, which a proxy may pass it on to;
    // RFC 9113 only forbids NUL, CR and LF), and doesn't start or end with whitespace (RFC 9113, section 8.2.1).
    bool valid_value(std::string_view value) noexcept
    {
        if (!is_valid_field_value(value)) return false;
        if (value.empty()) return true;

        const auto space = [](char c) { return c == ' ' || c == '\t'; };
        return !space(value.front()) && !space(value.back());
    }

    // connection_specific is whether a field is HTTP/1's connection management, which HTTP/2 forbids (RFC 9113,
    // section 8.2.2).
    bool connection_specific(std::string_view name) noexcept
    {
        return name == "connection" || name == "keep-alive" || name == "proxy-connection" || name == "transfer-encoding"
            || name == "upgrade";
    }

    std::optional<std::uint64_t> parse_length(std::string_view value) noexcept
    {
        if (value.empty() || value.size() > 19) return std::nullopt;

        std::uint64_t n = 0;
        for (const auto c : value)
        {
            if (c < '0' || c > '9') return std::nullopt;
            n = (n * 10) + static_cast<std::uint64_t>(c - '0');
        }
        return n;
    }

    // scanned_fields is what scan_fields() found in a header block.
    struct scanned_fields
    {
        std::optional<std::string_view> method;
        std::optional<std::string_view> scheme;
        std::optional<std::string_view> authority;
        std::optional<std::string_view> path;
        std::optional<std::string_view> protocol; // extended CONNECT's (RFC 8441, section 4)
        std::optional<std::string_view> status;
        std::optional<std::string_view> host;
        std::optional<std::uint64_t>    content_length;
    };

    // pseudo_slot is where a pseudo-header the side receives goes, or nullptr if it's not one it may receive.
    std::optional<std::string_view>* pseudo_slot(scanned_fields& out, std::string_view name, role side) noexcept
    {
        if (side == role::client) return name == ":status" ? &out.status : nullptr;

        if (name == ":method") return &out.method;
        if (name == ":scheme") return &out.scheme;
        if (name == ":authority") return &out.authority;
        if (name == ":path") return &out.path;
        if (name == ":protocol") return &out.protocol;
        return nullptr;
    }

    // scan_regular checks a regular (not pseudo-) field, adding what scan_fields() looks for to out, and returns
    // whether it's well-formed.
    bool scan_regular(const hpack::field& f, scanned_fields& out) noexcept
    {
        if (!valid_name(f.name) || connection_specific(f.name)) return false;
        if (f.name == "te" && f.value != "trailers") return false;

        if (f.name == "host")
        {
            // more than one would leave which host is meant to whoever reads them.
            if (out.host.has_value()) return false;
            out.host = f.value;
        }

        if (f.name == "content-length")
        {
            const auto length = parse_length(f.value);
            if (!length.has_value() || out.content_length.value_or(*length) != *length) return false;
            out.content_length = length;
        }

        return true;
    }

    // scan_fields checks a header block's fields (RFC 9113, section 8.2): pseudo-headers first (none in trailers),
    // each once, and only those the side receives; names and values valid; no connection-specific fields; at most one
    // Host; and Content-Lengths that agree. It returns what it found, or nullopt if they're malformed.
    std::optional<scanned_fields> scan_fields(std::span<const hpack::field> fields, role side, bool trailers)
    {
        scanned_fields out;
        bool           regular = false; // a regular field has been seen: pseudo-headers must come first

        for (const auto& f : fields)
        {
            if (!valid_value(f.value)) return std::nullopt;

            if (f.name.starts_with(':'))
            {
                auto* slot = pseudo_slot(out, f.name, side);
                if (regular || trailers || slot == nullptr || slot->has_value()) return std::nullopt;
                *slot = f.value;
                continue;
            }

            regular = true;
            if (!scan_regular(f, out)) return std::nullopt;
        }

        return out;
    }

    // valid_request is whether a request's pseudo-headers are (RFC 9113, section 8.3.1): CONNECT has only :method and
    // :authority; others :method, :scheme and a :path; and a Host agrees with :authority (host names ignore case). An
    // extended CONNECT (RFC 8441, section 4), if this side takes it (has sent ENABLE_CONNECT_PROTOCOL = 1), is CONNECT
    // with a :protocol (a token, as Upgrade's protocol names are), :scheme and a :path as other requests have, and
    // :authority; :protocol otherwise, or with any other method, is malformed.
    bool valid_request(const scanned_fields& f, bool extended_connect) noexcept
    {
        if (!f.method.has_value()) return false;

        if (f.protocol.has_value())
        {
            if (!extended_connect || *f.method != "CONNECT" || !is_valid_field_name(*f.protocol)
                || !f.authority.has_value() || !f.scheme.has_value() || !f.path.has_value() || f.path->empty())
                return false;
        }
        else if (*f.method == "CONNECT")
        {
            if (!f.authority.has_value() || f.scheme.has_value() || f.path.has_value()) return false;
        }
        else if (!f.scheme.has_value() || !f.path.has_value() || f.path->empty())
        {
            return false;
        }

        return !f.host.has_value() || !f.authority.has_value() || util::equal_ignore_case(*f.host, *f.authority);
    }

    // ended_short is whether a stream whose body has ended has less than its Content-Length promised (a response to
    // HEAD, or a 204 or 304, has none, whatever it says; a tunnel's never has one).
    bool ended_short(const auto& stream) noexcept
    {
        return stream.content_length.has_value() && stream.received != *stream.content_length
            && stream.kind != decltype(stream.kind)::head && !stream.no_body;
    }

    // request_head is what a client's session reads of a header block it sends, if it's a request's head: its method,
    // whether it has :protocol, and its Priority field, if it has one.
    struct request_head
    {
        std::optional<std::string_view> method; // none for a block that isn't a request's head (trailers)
        bool                            protocol = false;
        std::optional<priority>         said;
    };

    // read_request_head reads a header block a client sends. Its Priority field is read as the server reads it: its
    // lines joined (RFC 9651, section 4.2; in a string of their own only if there are several, which is rare), over RFC
    // 9218's default.
    request_head read_request_head(std::span<const hpack::field> fields)
    {
        request_head     out;
        std::size_t      lines = 0;
        std::string_view first;
        std::string      joined;
        for (const auto& f : fields)
        {
            if (f.name == ":method") out.method = f.value;
            if (f.name == ":protocol") out.protocol = true;
            if (f.name != "priority") continue;

            if (++lines == 1)
            {
                first = f.value;
                continue;
            }
            if (lines == 2) joined = first;
            joined += ", ";
            joined += f.value;
        }

        if (lines > 0) out.said = parse_priority(lines == 1 ? first : std::string_view{joined});
        return out;
    }

    std::optional<int> parse_status(std::string_view value) noexcept
    {
        if (value.size() != 3 || value[0] < '1' || value[0] > '5') return std::nullopt;
        if (!std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
        return ((value[0] - '0') * 100) + ((value[1] - '0') * 10) + (value[2] - '0');
    }

}

bool token_bucket::take(time_point now) noexcept
{
    if (last != time_point{})
    {
        const auto elapsed = std::chrono::duration<double>(now - last).count();
        tokens             = std::min(burst, tokens + (std::max(elapsed, 0.0) * rate));
    }
    last = now;

    if (tokens < 1) return false;
    tokens -= 1;
    return true;
}

session::session(role r, const session_config& config)
    : side{r}
    , config{config}
    , decoder{hpack::default_table_size,
              config.local.max_header_list_size == unlimited ? std::numeric_limits<std::size_t>::max()
                                                             : config.local.max_header_list_size}
    , encoder{hpack::indexing::automatic}
    , next_local_stream{r == role::client ? 1U : 2U}
{
    // NOTE: this->config, not the parameter of the same name.
    auto& local                 = this->config.local;
    local.enable_push           = false;
    local.no_rfc7540_priorities = true;
    if (side == role::client) local.enable_connect_protocol = false;
    local.initial_window_size = std::clamp<std::uint32_t>(local.initial_window_size, 65'535, max_window);
    local.max_frame_size      = std::clamp(local.max_frame_size, min_max_frame_size, max_max_frame_size);

    // as many CONTINUATIONs as the largest header block this side takes needs, if that's more (a block's encoded size
    // is limited by max_header_list_size too), in frames of the least MAX_FRAME_SIZE: the peer may send frames that
    // small whatever this side's (and does before it has this side's SETTINGS).
    const auto frames = (std::uint64_t{local.max_header_list_size} + min_max_frame_size - 1) / min_max_frame_size;
    this->config.max_continuations = std::max(this->config.max_continuations, static_cast<std::size_t>(frames));

    if (side == role::client)
    {
        const auto bytes = std::as_bytes(std::span{preface});
        output.insert(output.end(), bytes.begin(), bytes.end());
    }

    // the encoder's table starts at the peer's initial SETTINGS_HEADER_TABLE_SIZE, unless the cap is smaller: then the
    // first block says so.
    encoder.set_max_table_size(std::min(this->config.encoder_table_size, peer.header_table_size));

    encode_settings(local, settings{}, output);

    if (this->config.connection_window > 65'535)
    {
        const auto window      = std::min<std::uint32_t>(this->config.connection_window, max_window);
        connection_recv_window = window;
        encode_window_update(0, window - 65'535, output);
    }
}

std::expected<std::size_t, std::error_code>
session::receive(std::span<const std::byte> in, session_events& events, time_point now)
{
    if (connection_failed) return std::unexpected(failure);

    std::size_t used = 0;

    // a server's client starts with the preface (RFC 9113, section 3.4).
    if (side == role::server && !preface_received)
    {
        const auto expected = std::as_bytes(std::span{preface});
        const auto n        = std::min(in.size(), expected.size());
        if (!std::ranges::equal(in.first(n), expected.first(n)))
            return std::unexpected(connection_error(error_code::protocol_error, "not HTTP/2"));
        if (n < expected.size()) return 0;

        preface_received = true;
        used             = expected.size();
    }

    while (!connection_failed)
    {
        const auto frame = receive_frame(in.subspan(used), events, now);
        if (!frame.has_value()) return std::unexpected(frame.error());
        if (*frame == 0) break;
        used += *frame;
    }

    if (connection_failed) return std::unexpected(failure);
    return used;
}

std::expected<std::size_t, std::error_code>
session::receive_frame(std::span<const std::byte> in, session_events& events, time_point now)
{
    if (in.size() < frame_header_size) return 0;

    const auto h = decode_frame_header(in.first<frame_header_size>());
    if (h.length > config.local.max_frame_size) return std::unexpected(connection_error(error_code::frame_size_error));
    if (in.size() < frame_header_size + h.length) return 0;

    const auto payload = in.subspan(frame_header_size, h.length);

    // the peer's first frame is its SETTINGS (RFC 9113, section 3.4).
    if (!settings_received
        && (h.type != static_cast<std::uint8_t>(frame_type::settings) || (h.flags & flags::ack) != 0))
        return std::unexpected(connection_error(error_code::protocol_error, "expected SETTINGS"));

    // a header block's frames come together, with nothing in between (RFC 9113, section 6.10).
    if (pending_block.has_value()
        && (h.type != static_cast<std::uint8_t>(frame_type::continuation) || h.stream != pending_block->stream))
        return std::unexpected(connection_error(error_code::protocol_error, "expected CONTINUATION"));

    std::error_code err;
    switch (static_cast<frame_type>(h.type))
    {
    case frame_type::data: err = on_data(h, payload, events, now); break;
    case frame_type::headers: err = on_headers(h, payload, events, now); break;
    case frame_type::priority: err = on_priority(h, payload, events, now); break;
    case frame_type::rst_stream: err = on_rst_stream(h, payload, events, now); break;
    case frame_type::settings: err = on_settings(h, payload, events); break;
    case frame_type::ping: err = on_ping(h, payload, events); break;
    case frame_type::goaway: err = on_goaway(h, payload, events); break;
    case frame_type::window_update: err = on_window_update(h, payload, events, now); break;
    case frame_type::continuation: err = on_continuation(h, payload, events, now); break;
    case frame_type::priority_update: err = on_priority_update(h, payload, events, now); break;
    case frame_type::origin: on_origin(h, payload, events); break;

    // a client sends ENABLE_PUSH 0, and a client can't push.
    case frame_type::push_promise: err = connection_error(error_code::protocol_error, "PUSH_PROMISE"); break;

    // unknown frame types are ignored (RFC 9113, section 4.1).
    default: break;
    }

    if (err) return std::unexpected(err);
    return frame_header_size + h.length;
}

std::error_code
session::on_data(const frame_header& h, std::span<const std::byte> payload, session_events& events, time_point now)
{
    if (h.stream == 0) return connection_error(error_code::protocol_error);

    const auto data = unpad(payload, h.flags);
    if (!data.has_value()) return connection_error(error_code::protocol_error, "padding");

    // the whole payload counts against the windows, padding and all (RFC 9113, section 6.9).
    const auto length = static_cast<std::int64_t>(payload.size());
    if (length > connection_recv_window) return connection_error(error_code::flow_control_error);
    connection_recv_window -= length;

    auto it = streams.find(h.stream);
    if (it == streams.end() || it->second.remote_closed || !it->second.head_received)
    {
        // what the stream would have taken is given back to the connection at once.
        give_back(h.stream, payload.size());

        if (it == streams.end() && !closed(h.stream)) return connection_error(error_code::protocol_error, "idle");
        if (it == streams.end() && ignores(h.stream)) return {};
        return stream_error(h.stream,
                            it != streams.end() && !it->second.remote_closed ? error_code::protocol_error
                                                                             : error_code::stream_closed,
                            &events,
                            now);
    }

    auto& s = it->second;
    if (length > s.recv_window)
        return data_error(h.stream, payload.size(), error_code::flow_control_error, events, now);
    s.recv_window -= length;

    const bool end = (h.flags & flags::end_stream) != 0;

    s.received += data->size();
    const bool too_long  = s.content_length.has_value() && s.received > *s.content_length;
    const bool too_short = end && ended_short(s);
    if (too_long || too_short) return data_error(h.stream, payload.size(), error_code::protocol_error, events, now);

    // an empty frame that doesn't end the stream does nothing (CVE-2019-9518, empty frames flood).
    if (data->empty() && !end)
    {
        if (auto err = glitch(now)) return err;
    }

    // padding is given back at once; the data once the application has consumed it.
    if (const auto padding = payload.size() - data->size(); padding > 0) give_back(h.stream, padding);

    events.on_data(h.stream, *data, end);
    if (end) close_remote(h.stream);
    return {};
}

std::error_code
session::on_headers(const frame_header& h, std::span<const std::byte> payload, session_events& events, time_point now)
{
    if (h.stream == 0) return connection_error(error_code::protocol_error);

    auto fragment = unpad(payload, h.flags);
    if (!fragment.has_value()) return connection_error(error_code::protocol_error, "padding");

    // RFC 7540's priority, which is deprecated, and ignored; but, as in PRIORITY, a stream can't depend on itself.
    bool self_dependent = false;
    if ((h.flags & flags::priority) != 0)
    {
        if (fragment->size() < 5) return connection_error(error_code::frame_size_error);
        self_dependent = (read32(*fragment) & max_stream_id) == h.stream;
        fragment       = fragment->subspan(5);
    }

    if (fragment->size() > config.local.max_header_list_size)
        return connection_error(error_code::enhance_your_calm, "header block too large");

    header_block block{
        .stream         = h.stream,
        .end_stream     = (h.flags & flags::end_stream) != 0,
        .self_dependent = self_dependent,
        .fragment       = {fragment->begin(), fragment->end()},
    };

    if ((h.flags & flags::end_headers) != 0) return end_header_block(block, events, now);

    pending_block = std::move(block);
    return {};
}

std::error_code session::on_continuation(const frame_header&        h,
                                         std::span<const std::byte> payload,
                                         session_events&            events,
                                         time_point                 now)
{
    // (receive_frame has checked it's the pending block's.)
    if (!pending_block.has_value()) return connection_error(error_code::protocol_error, "CONTINUATION");

    // a CONTINUATION flood (VU#421644): too many frames, or too much.
    auto& block = *pending_block;
    if (++block.continuations > config.max_continuations
        || block.fragment.size() + payload.size() > config.local.max_header_list_size)
        return connection_error(error_code::enhance_your_calm, "header block too large");

    block.fragment.insert(block.fragment.end(), payload.begin(), payload.end());

    const bool end = (h.flags & flags::end_headers) != 0;
    if (payload.empty() && !end)
    {
        if (auto err = glitch(now)) return err;
    }

    if (!end) return {};

    const auto complete = std::move(block);
    pending_block.reset();
    return end_header_block(complete, events, now);
}

std::error_code session::end_header_block(const header_block& block, session_events& events, time_point now)
{
    // decoded whatever becomes of it, to keep HPACK's dynamic table in step (RFC 9113, section 4.3).
    const auto decoded = decode_block(block.fragment);
    if (decoded == std::errc::invalid_argument) return connection_error(error_code::compression_error);

    const auto id     = block.stream;
    const auto stream = stream_for_block(id, now);

    // a PRIORITY_UPDATE that came before the stream opened (a server's): reported after its head, if it's well-formed,
    // or dropped with it.
    const auto update = idle_priorities.empty() ? std::nullopt : take_idle_priority(id);

    if (!stream.has_value()) return stream.error();
    if (*stream == nullptr) return {}; // ignored, or refused

    auto& s = **stream;
    if (s.remote_closed) return stream_error(id, error_code::stream_closed, &events, now);

    // (only once it's decoded, and its stream found or opened, so HPACK and the streams stay in step.)
    if (block.self_dependent) return stream_error(id, error_code::protocol_error, &events, now);

    // a header list over the limit: a request's head is reported without its fields, for the server to answer (431,
    // RFC 9113 section 10.5.1, rather than a reset), its stream open and having its head (so what follows is its body,
    // and trailers); trailers, which may come with a response under way, and a response's head are refused, as the
    // stream's error. Either way it's a glitch, so a peer that sends nothing else is stopped as one that's reset is.
    if (decoded == std::errc::value_too_large)
    {
        if (side == role::client || s.head_received) return stream_error(id, error_code::protocol_error, &events, now);
        if (auto err = glitch(now)) return err;

        // (a PRIORITY_UPDATE kept for it is dropped, as for a malformed head.)
        s.head_received = true;
        s.known         = true;
        events.on_header_list_too_large(id, block.end_stream);
        if (block.end_stream) close_remote(id);
        return {};
    }

    const auto checked = validate(s, block.end_stream);
    if (!checked.has_value()) return stream_error(id, error_code::protocol_error, &events, now);

    if (checked->kind == block_kind::head)
    {
        s.head_received  = true;
        s.known          = true;
        s.content_length = checked->content_length;
        s.no_body        = checked->status == 204 || checked->status == 304;
        s.kind           = checked->stream_kind;

        // a head that ends the stream promises no body.
        if (block.end_stream && s.content_length.value_or(0) != 0 && s.kind != exchange::head && !s.no_body)
            return stream_error(id, error_code::protocol_error, &events, now);
    }
    else if (checked->kind == block_kind::trailers && ended_short(s))
    {
        // trailers end the stream, so its body must be as long as its Content-Length said (as on_data checks when
        // DATA ends it).
        return stream_error(id, error_code::protocol_error, &events, now);
    }

    events.on_headers(id, checked->kind, block_fields, block.end_stream);

    // (the most recent signal, so it overrides the request's Priority field: RFC 9218, section 7.1.)
    if (update.has_value() && checked->kind == block_kind::head) events.on_priority_update(id, *update);

    if (block.end_stream) close_remote(id);
    return {};
}

std::error_code session::decode_block(std::span<const std::byte> fragment)
{
    block_used     = 0;
    const auto err = decoder.decode(fragment,
                                    [this](const hpack::field& f)
                                    {
                                        if (block_used == block_storage.size()) block_storage.emplace_back();
                                        if (block_used == block_never_indexed.size())
                                            block_never_indexed.push_back(false);

                                        auto& [name, value] = block_storage[block_used];
                                        name.assign(f.name);
                                        value.assign(f.value);
                                        block_never_indexed[block_used] = f.never_indexed;
                                        ++block_used;
                                    });

    block_fields.clear();
    for (std::size_t i = 0; i < block_used; ++i)
    {
        const auto& [name, value] = block_storage[i];
        block_fields.push_back({.name = name, .value = value, .never_indexed = block_never_indexed[i]});
    }

    return err;
}

std::expected<session::stream_state*, std::error_code> session::stream_for_block(stream_id id, time_point now)
{
    if (const auto it = streams.find(id); it != streams.end()) return &it->second;

    // (it's not open, so it's idle or closed.)
    const bool idle = !closed(id);

    // only a client's request opens a stream.
    if (idle && (side == role::client || !peer_initiated(id)))
        return std::unexpected(connection_error(error_code::protocol_error));

    // after this side's GOAWAY, the peer's new streams are ignored (RFC 9113, section 6.8): not processed, nor counted
    // as glitches (the peer may not have seen the GOAWAY yet), whichever of their frames it is.
    if (side == role::server && peer_initiated(id) && local_goaway.has_value() && id > *local_goaway)
    {
        highest_peer_stream = std::max(highest_peer_stream, id);
        return nullptr;
    }

    if (!idle)
    {
        // e.g. what the peer sent before it saw this side's reset.
        if (was_reset(id)) return nullptr;

        // a stream closed for good, or one whose reset is too old to remember: as it may be the latter, it's the
        // stream's error rather than the connection's (as Go's).
        if (auto err = stream_error(id, error_code::stream_closed, nullptr, now)) return std::unexpected(err);
        return nullptr;
    }

    // a new stream (RFC 9113, section 5.1.1).
    highest_peer_stream = id;

    if (open_peer_streams >= config.local.max_concurrent_streams)
    {
        encode_rst_stream(id, static_cast<std::uint32_t>(error_code::refused_stream), output);
        remember_reset(id);
        if (auto err = glitch(now)) return std::unexpected(err);
        return nullptr;
    }

    stream_state s;
    s.send_window = peer.initial_window_size;
    s.recv_window = config.local.initial_window_size;
    ++open_peer_streams;
    return &streams.emplace(id, s).first->second;
}

std::optional<session::validated> session::validate(const stream_state& s, bool end_stream) const
{
    validated out;
    out.stream_kind = s.kind;

    const bool trailers = s.head_received;
    if (trailers)
    {
        // trailers end the stream, and have no pseudo-headers (RFC 9113, section 8.1); a tunnel has none (only DATA
        // and the frames that manage the stream follow its head, RFC 9113 section 8.5).
        if (!end_stream || s.kind == exchange::tunnel) return std::nullopt;
        out.kind = block_kind::trailers;
    }

    const auto scanned = scan_fields(block_fields, side, trailers);
    if (!scanned.has_value()) return std::nullopt;

    out.content_length = scanned->content_length;
    if (trailers) return out;

    if (side == role::server)
    {
        if (!valid_request(*scanned, config.local.enable_connect_protocol)) return std::nullopt;

        // a CONNECT request has no content (RFC 9110, section 9.3.6): its stream's DATA is the tunnel's, which a
        // Content-Length doesn't describe, so it's not held to one.
        if (scanned->method == "CONNECT")
        {
            out.stream_kind    = exchange::tunnel;
            out.content_length = std::nullopt;
        }
        return out;
    }

    // a response (RFC 9113, section 8.3.2).
    const auto code = scanned->status.and_then(parse_status);
    if (!code.has_value() || *code == 101) return std::nullopt;

    out.status = *code;
    if (*code < 200)
    {
        // an informational response is followed by the final one.
        if (end_stream) return std::nullopt;
        out.kind = block_kind::informational;
    }
    else if (s.kind == exchange::connect && *code < 300)
    {
        // a 2xx to CONNECT opens the tunnel: a client ignores its Content-Length (RFC 9110, section 9.3.6).
        out.stream_kind    = exchange::tunnel;
        out.content_length = std::nullopt;
    }

    return out;
}

std::error_code
session::on_priority(const frame_header& h, std::span<const std::byte> payload, session_events& events, time_point now)
{
    if (h.stream == 0) return connection_error(error_code::protocol_error);

    // ignored (RFC 9113 deprecated RFC 7540's priorities), but it must be 5 bytes, and a stream can't depend on itself.
    const bool wrong_size = payload.size() != 5;
    if (!wrong_size && (read32(payload) & max_stream_id) != h.stream) return {};

    // PRIORITY may come for a stream that isn't open (idle, or closed), which can't be reset (RST_STREAM on an idle
    // stream is a connection error, RFC 9113 section 6.4): it's only counted.
    if (!streams.contains(h.stream)) return glitch(now);
    return stream_error(h.stream, wrong_size ? error_code::frame_size_error : error_code::protocol_error, &events, now);
}

std::error_code session::on_rst_stream(const frame_header&        h,
                                       std::span<const std::byte> payload,
                                       session_events&            events,
                                       time_point                 now)
{
    if (h.stream == 0) return connection_error(error_code::protocol_error);
    if (payload.size() != 4) return connection_error(error_code::frame_size_error);

    const auto it = streams.find(h.stream);
    if (it == streams.end())
    {
        if (!closed(h.stream)) return connection_error(error_code::protocol_error, "idle");
        return {};
    }

    const bool known = it->second.known;
    forget(h.stream);

    // Rapid Reset (CVE-2023-44487): a client opening streams and resetting them at once.
    if (side == role::server && !config.peer_resets.take(now))
        return connection_error(error_code::enhance_your_calm, "too many resets");

    if (known) events.on_stream_reset(h.stream, make_error_code(static_cast<error_code>(read32(payload))));
    return {};
}

std::error_code session::on_settings(const frame_header& h, std::span<const std::byte> payload, session_events& events)
{
    if (h.stream != 0) return connection_error(error_code::protocol_error);

    if ((h.flags & flags::ack) != 0)
    {
        if (!payload.empty()) return connection_error(error_code::frame_size_error);

        // this side's settings are in effect: the peer's encoder now knows the table's size.
        decoder.set_max_table_size(config.local.header_table_size);
        return {};
    }

    if (payload.size() % 6 != 0) return connection_error(error_code::frame_size_error);
    if (payload.size() / 6 > config.max_settings)
        return connection_error(error_code::enhance_your_calm, "too many settings");

    const bool first  = !settings_received;
    settings_received = true;

    const auto previous_window = static_cast<std::int64_t>(peer.initial_window_size);
    for (auto rest = payload; !rest.empty(); rest = rest.subspan(6))
    {
        if (auto err = apply_setting(read16(rest), read32(rest.subspan(2)), first)) return err;
    }

    // a change of INITIAL_WINDOW_SIZE moves every stream's window by as much (RFC 9113, section 6.9.2).
    const auto delta = static_cast<std::int64_t>(peer.initial_window_size) - previous_window;
    for (auto& [id, s] : streams)
    {
        s.send_window += delta;
        if (s.send_window > max_window) return connection_error(error_code::flow_control_error);
    }

    encode_settings_ack(output);
    if (auto err = count_reply()) return err;

    events.on_settings(peer);
    if (delta > 0) events.on_window_update(0);
    return {};
}

std::error_code session::apply_setting(std::uint16_t id, std::uint32_t value, bool first)
{
    switch (static_cast<setting_id>(id))
    {
    case setting_id::header_table_size:
        // NOTE: the encoder may use less than the peer offers (RFC 7541, section 4.2): it's held to its cap, and so
        //       only has to signal a change when the peer's setting goes below that (the smallest, then the final).
        peer.header_table_size = value;
        encoder.set_max_table_size(std::min(config.encoder_table_size, value));
        break;

    case setting_id::enable_push:
        // a server may only send 0 (RFC 9113, section 6.5.2).
        if (value > 1 || (side == role::client && value == 1)) return connection_error(error_code::protocol_error);
        peer.enable_push = value == 1;
        break;

    case setting_id::max_concurrent_streams: peer.max_concurrent_streams = value; break;

    case setting_id::initial_window_size:
        if (value > max_window) return connection_error(error_code::flow_control_error);
        peer.initial_window_size = value;
        break;

    case setting_id::max_frame_size:
        if (value < min_max_frame_size || value > max_max_frame_size)
            return connection_error(error_code::protocol_error);
        peer.max_frame_size = value;
        break;

    case setting_id::max_header_list_size: peer.max_header_list_size = value; break;

    case setting_id::enable_connect_protocol:
        // 0 or 1 from either side (RFC 8441, section 3), but it means something only from a server: that a client may
        // send extended CONNECT. So a client's is ignored. A server can't withdraw it once it's 1: a later 0 is an
        // error (a later SETTINGS that leaves it out changes nothing).
        if (value > 1) return connection_error(error_code::protocol_error, "ENABLE_CONNECT_PROTOCOL");
        if (side == role::server) break;
        if (value == 0 && peer.enable_connect_protocol)
            return connection_error(error_code::protocol_error, "ENABLE_CONNECT_PROTOCOL withdrawn");
        peer.enable_connect_protocol = value == 1;
        break;

    case setting_id::no_rfc7540_priorities:
        // 0 or 1, in the first SETTINGS, and never changed after (RFC 9218, section 2.1, whose "MAY" treat a change as
        // a connection error this takes: Go's checks only the value). A later SETTINGS that leaves it out changes
        // nothing; one that sends 1 when the first didn't send it changes it from its initial 0. Nothing else depends
        // on it: neither side sends RFC 7540's signals, and a server ignores them whatever the client says.
        if (value > 1 || (!first && (value == 1) != peer.no_rfc7540_priorities))
            return connection_error(error_code::protocol_error, "NO_RFC7540_PRIORITIES");
        peer.no_rfc7540_priorities = value == 1;
        break;

    // unknown settings are ignored.
    default: break;
    }

    return {};
}

std::error_code session::on_ping(const frame_header& h, std::span<const std::byte> payload, session_events& events)
{
    if (h.stream != 0) return connection_error(error_code::protocol_error);
    if (payload.size() != 8) return connection_error(error_code::frame_size_error);

    const auto opaque = payload.first<8>();
    if ((h.flags & flags::ack) != 0)
    {
        events.on_ping_ack(opaque);
        return {};
    }

    encode_ping(opaque, true, output);
    return count_reply();
}

std::error_code session::on_goaway(const frame_header& h, std::span<const std::byte> payload, session_events& events)
{
    if (h.stream != 0) return connection_error(error_code::protocol_error);
    if (payload.size() < 8) return connection_error(error_code::frame_size_error);

    // a later GOAWAY mustn't raise the last stream; if one does, the lower stands.
    const auto last  = std::min(read32(payload) & max_stream_id, peer_goaway.value_or(max_stream_id));
    const auto code  = make_error_code(static_cast<error_code>(read32(payload.subspan(4))));
    const auto debug = std::string_view{reinterpret_cast<const char*>(payload.data()) + 8, payload.size() - 8};

    peer_goaway = last;
    events.on_goaway(last, code, debug);

    // this side's streams after the last weren't processed, and may be retried (RFC 9113, section 6.8).
    std::vector<stream_id> unprocessed;
    for (const auto& [id, s] : streams)
    {
        if (!peer_initiated(id) && id > last) unprocessed.push_back(id);
    }
    std::ranges::sort(unprocessed);

    for (const auto id : unprocessed)
    {
        const bool known = streams.at(id).known;
        forget(id);
        if (known) events.on_stream_reset(id, make_error_code(error_code::refused_stream));
    }

    return {};
}

std::error_code session::on_window_update(const frame_header&        h,
                                          std::span<const std::byte> payload,
                                          session_events&            events,
                                          time_point                 now)
{
    if (payload.size() != 4) return connection_error(error_code::frame_size_error);
    const auto increment = static_cast<std::int64_t>(read32(payload) & max_stream_id);

    if (h.stream == 0)
    {
        if (increment == 0) return connection_error(error_code::protocol_error);
        if (connection_send_window + increment > max_window) return connection_error(error_code::flow_control_error);

        connection_send_window += increment;
        events.on_window_update(0);
        return {};
    }

    const auto it = streams.find(h.stream);
    if (it == streams.end())
    {
        if (!closed(h.stream)) return connection_error(error_code::protocol_error, "idle");
        return {};
    }

    if (increment == 0) return stream_error(h.stream, error_code::protocol_error, &events, now);
    if (it->second.send_window + increment > max_window)
        return stream_error(h.stream, error_code::flow_control_error, &events, now);

    it->second.send_window += increment;
    events.on_window_update(h.stream);
    return {};
}

std::error_code session::on_priority_update(const frame_header&        h,
                                            std::span<const std::byte> payload,
                                            session_events&            events,
                                            time_point                 now)
{
    // only a client sends it (RFC 9218, section 7.1), on stream 0, with a stream id (31 bits) and a Priority field's
    // value, which may be empty.
    if (side == role::client) return connection_error(error_code::protocol_error, "PRIORITY_UPDATE");
    if (h.stream != 0) return connection_error(error_code::protocol_error);
    if (payload.size() < 4) return connection_error(error_code::frame_size_error);

    // a stream id of 0 is an error, and so is an even one: a push stream, which this side never opens, so it's idle
    // (for which RFC 9218 makes it an error) and always will be.
    const auto id = read32(payload) & max_stream_id;
    if (id == 0 || !peer_initiated(id)) return connection_error(error_code::protocol_error, "PRIORITY_UPDATE stream");

    const auto it   = streams.find(id);
    const bool idle = it == streams.end() && !closed(id);

    // after this side's GOAWAY, the peer's new streams won't be processed, and its frames for them are ignored, as
    // ever (not counted: the peer may not have seen the GOAWAY yet).
    if (local_goaway.has_value() && id > *local_goaway && it == streams.end()) return {};

    // a closed stream's is ignored (e.g. a client reprioritizing a response that ended meanwhile), but counted, so
    // a flood of them can't go on for free.
    if (it == streams.end() && !idle) return glitch(now);

    // a value that doesn't parse is ignored too, rather than the stream's error (Go's) or the connection's (RFC 9218
    // allows either), and counted. It's parsed to a priority now, so what's kept is two bytes, not the frame's value.
    // NOTE: parsed twice (once to tell whether it parses, as parse_priority() doesn't say), only for this frame.
    const auto value = std::string_view{reinterpret_cast<const char*>(payload.data()) + 4, payload.size() - 4};
    if (!sf::parse_dictionary(value, [](std::string_view, const sf::member&) {})) return glitch(now);

    // a complete set: what it leaves out is the default, not what the stream had.
    const auto p = parse_priority(value);

    if (!idle)
    {
        if (it->second.known) events.on_priority_update(id, p);
        return {};
    }

    // a stream not yet open: the latest is kept, until its head arrives. RFC 9218 bounds them, with the streams that
    // are open, by MAX_CONCURRENT_STREAMS, as a connection error; and this side by max_idle_priorities, past which
    // they're ignored (and counted).
    const auto kept = std::ranges::find(idle_priorities, id, &std::pair<stream_id, priority>::first);
    if (kept != idle_priorities.end())
    {
        kept->second = p;
        return {};
    }

    if (open_peer_streams + idle_priorities.size() >= config.local.max_concurrent_streams)
        return connection_error(error_code::protocol_error, "PRIORITY_UPDATE for too many streams");
    if (idle_priorities.size() >= max_idle_priorities) return glitch(now);

    idle_priorities.emplace_back(id, p);
    return {};
}

void session::on_origin(const frame_header& h, std::span<const std::byte> payload, session_events& events) const
{
    // a server ignores it (RFC 8336, section 2.2), and so does a client on a stream other than 0, or with any of the
    // flags 0x1 to 0x8 set (its Appendix B, step 4; flags are left to future updates): not counted as glitches, as the
    // RFC has them ignored (and unknown frames aren't counted either).
    constexpr std::uint8_t reserved = 0x0f;
    if (side == role::server || h.stream != 0 || (h.flags & reserved) != 0) return;

    // entries are a length (16 bits) and an origin's ASCII serialization: one whose length runs past the payload
    // can't be parsed, nor can what follows it, so they're ignored (RFC 8336, section 2.2: "If parsing fails, the
    // field MUST be ignored"). NOTE: views of the payload (at most MAX_FRAME_SIZE, so at most 8192 entries), in a
    // vector of their own, allocated only for this frame, which a server sends once or so a connection.
    std::vector<std::string_view> origins;
    while (payload.size() >= 2)
    {
        const std::size_t length = read16(payload);
        if (length > payload.size() - 2) break;

        origins.emplace_back(reinterpret_cast<const char*>(payload.data()) + 2, length);
        payload = payload.subspan(2 + length);
    }
    events.on_origin(origins);
}

std::optional<priority> session::take_idle_priority(stream_id stream)
{
    std::optional<priority> taken;
    std::erase_if(idle_priorities,
                  [&](const std::pair<stream_id, priority>& kept)
                  {
                      if (kept.first == stream) taken = kept.second;
                      return kept.first <= highest_peer_stream;
                  });
    return taken;
}

bool session::can_open_stream(std::size_t reserved) const noexcept
{
    const auto limit = settings_received ? peer.max_concurrent_streams : assumed_max_concurrent_streams;
    return side == role::client && !connection_failed && !peer_goaway.has_value() && !local_goaway.has_value()
        && open_local_streams + reserved < limit && next_local_stream + (2 * reserved) <= max_stream_id;
}

std::expected<stream_id, std::error_code> session::open_stream()
{
    if (!can_open_stream()) return std::unexpected(make_error_code(error_code::refused_stream));

    const auto id = next_local_stream;
    next_local_stream += 2;

    stream_state s;
    s.known       = true;
    s.send_window = peer.initial_window_size;
    s.recv_window = config.local.initial_window_size;
    streams.emplace(id, s);
    ++open_local_streams;
    return id;
}

std::error_code session::submit_headers(stream_id stream, std::span<const hpack::field> fields, bool end_stream)
{
    const auto it = streams.find(stream);
    if (connection_failed || it == streams.end() || it->second.local_closed)
        return make_error_code(error_code::stream_closed);

    // a response to HEAD has no body, whatever its Content-Length says, and one to CONNECT may open a tunnel. And a
    // request's Priority field is the first thing its stream says of its priority (see submit_priority_update()).
    // (Trailers have no :method.)
    if (const auto head = side == role::client ? read_request_head(fields) : request_head{}; head.method.has_value())
    {
        // extended CONNECT only once the server has said it takes it (RFC 8441, section 4): until then, the server
        // would take it as malformed. Nothing has been sent on the stream (a request's head is its first frame), so
        // it's forgotten rather than reset.
        if (head.protocol && !peer.enable_connect_protocol)
        {
            forget(stream);
            return std::make_error_code(std::errc::invalid_argument);
        }

        auto& s = it->second;
        if (head.method == "HEAD") s.kind = exchange::head;
        else if (head.method == "CONNECT") s.kind = exchange::connect;
        if (head.said.has_value()) s.said = *head.said;
    }

    encoded_block.clear();
    encoder.encode(fields, encoded_block);
    encode_headers(stream, encoded_block, end_stream, peer.max_frame_size, output);

    if (end_stream) close_local(stream);
    return {};
}

std::expected<std::size_t, std::error_code>
session::submit_data(stream_id stream, std::span<const std::byte> data, bool end_stream)
{
    const auto it = streams.find(stream);
    if (connection_failed || it == streams.end() || it->second.local_closed)
        return std::unexpected(make_error_code(error_code::stream_closed));

    auto&       s    = it->second;
    std::size_t sent = 0;
    while (true)
    {
        const auto window = std::max<std::int64_t>(std::min(s.send_window, connection_send_window), 0);
        const auto chunk  = std::min(
            {data.size() - sent, static_cast<std::size_t>(window), static_cast<std::size_t>(peer.max_frame_size)});
        const bool last = sent + chunk == data.size();
        if (chunk == 0 && !(last && end_stream)) break;

        encode_data(stream, data.subspan(sent, chunk), last && end_stream, output);
        s.send_window -= static_cast<std::int64_t>(chunk);
        connection_send_window -= static_cast<std::int64_t>(chunk);
        sent += chunk;

        if (last)
        {
            if (end_stream) close_local(stream);
            break;
        }
    }

    return sent;
}

std::error_code session::submit_origin(std::span<const std::string> origins)
{
    if (side == role::client) return std::make_error_code(std::errc::invalid_argument);
    if (connection_failed) return failure;

    // each entry is 2 bytes and its origin, and every one must fit a frame by itself (the peer's MAX_FRAME_SIZE is at
    // least 16384, which an origin, a scheme and a host of at most 253 bytes and a port, never comes near).
    const std::size_t room = peer.max_frame_size;
    const auto        fits = [room](const std::string& o) { return o.size() <= 0xffff && 2 + o.size() <= room; };
    if (!std::ranges::all_of(origins, fits)) return std::make_error_code(std::errc::invalid_argument);

    // as many to a frame as fit, in order.
    std::size_t first = 0;
    do
    {
        std::size_t last   = first;
        std::size_t length = 0;
        while (last < origins.size() && length + 2 + origins[last].size() <= room) length += 2 + origins[last++].size();

        encode_origin(origins.subspan(first, last - first), output);
        first = last;
    }
    while (first < origins.size());
    return {};
}

std::expected<bool, std::error_code> session::submit_priority_update(stream_id stream, priority p)
{
    // only a client sends it (RFC 9218, section 7.1): a server's session takes one as a connection error.
    if (side == role::server) return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    // a stream the server's ended (or that's gone) has no response left to reprioritize: the server would ignore it
    // (and count it, as a glitch, as this side does).
    const auto it = streams.find(stream);
    if (connection_failed || it == streams.end() || it->second.remote_closed) return false;

    // past 7 is 7, as the schedulers take it (the server would ignore an urgency past 7, keeping the default).
    p.urgency = std::min<std::uint8_t>(p.urgency, 7);

    auto&      s       = it->second;
    const auto pending = std::ranges::find(pending_updates, stream, &pending_update::stream);
    if (p == s.said) return true;

    if (pending == pending_updates.end())
    {
        pending_updates.push_back({.stream = stream, .at = output.size(), .before = s.said});
        write_priority_update(stream, p, output.size(), 0);
        s.said = p;
        return true;
    }

    // one's waiting: it says this instead, or, if this is what was said before it, it's taken back.
    const auto at = pending->at;
    const auto old =
        frame_header_size + decode_frame_header(std::span{output}.subspan(at).first<frame_header_size>()).length;
    if (p == pending->before)
    {
        take_back_update(stream);
        return true;
    }

    write_priority_update(stream, p, at, old);
    s.said = p;
    return true;
}

void session::write_priority_update(stream_id stream, priority p, std::size_t at, std::size_t old)
{
    // NOTE: encoded_block is only used within a call (as submit_headers() encodes a block), so it's reused here.
    encoded_block.clear();
    encode_priority_update(stream, to_string(p), encoded_block);

    const auto first = output.begin() + static_cast<std::ptrdiff_t>(at);
    if (old == encoded_block.size())
    {
        std::ranges::copy(encoded_block, first);
        return;
    }

    if (old == 0)
    {
        output.insert(first, encoded_block.begin(), encoded_block.end());
    }
    else
    {
        output.erase(first, first + static_cast<std::ptrdiff_t>(old));
        output.insert(output.begin() + static_cast<std::ptrdiff_t>(at), encoded_block.begin(), encoded_block.end());
    }

    // the frames after it moved.
    for (auto& u : pending_updates)
    {
        if (u.at > at) u.at = u.at + encoded_block.size() - old;
    }
}

void session::take_back_update(stream_id stream)
{
    // NOTE: none, nearly always.
    if (pending_updates.empty()) return;

    const auto pending = std::ranges::find(pending_updates, stream, &pending_update::stream);
    if (pending == pending_updates.end()) return;

    const auto at = pending->at;
    const auto length =
        frame_header_size + decode_frame_header(std::span{output}.subspan(at).first<frame_header_size>()).length;
    if (const auto it = streams.find(stream); it != streams.end()) it->second.said = pending->before;
    pending_updates.erase(pending);

    const auto first = output.begin() + static_cast<std::ptrdiff_t>(at);
    output.erase(first, first + static_cast<std::ptrdiff_t>(length));
    for (auto& u : pending_updates)
    {
        if (u.at > at) u.at -= length;
    }
}

void session::reset(stream_id stream, error_code code)
{
    if (!streams.contains(stream)) return;

    encode_rst_stream(stream, static_cast<std::uint32_t>(code), output);
    remember_reset(stream);
    forget(stream);
}

void session::consume(stream_id stream, std::size_t n) { give_back(stream, n); }

void session::widen_window(stream_id stream, std::uint32_t window)
{
    const auto it = streams.find(stream);
    if (it == streams.end() || it->second.remote_closed || window <= config.local.initial_window_size) return;

    // NOTE: what's outstanding (the window left, and what's consumed but not yet given back) is the initial window
    //       until now, so it's window after: no more than a window can be (RFC 9113, section 6.9.1).
    auto&      s    = it->second;
    const auto more = std::min<std::int64_t>(window, max_window) - config.local.initial_window_size;
    encode_window_update(stream, static_cast<std::uint32_t>(more), output);
    s.recv_window += more;
}

void session::give_back(stream_id stream, std::size_t n)
{
    // the connection's window, once half of it has been consumed, or as much as the peer has left of it (so a stream
    // being read isn't held up by others' data waiting to be read, which the window is large enough for).
    connection_unconsumed += n;
    if (connection_unconsumed >= std::max<std::uint32_t>(config.connection_window, 65'535) / 2
        || std::cmp_greater_equal(connection_unconsumed, connection_recv_window))
    {
        encode_window_update(0, static_cast<std::uint32_t>(connection_unconsumed), output);
        connection_recv_window += static_cast<std::int64_t>(connection_unconsumed);
        connection_unconsumed = 0;
    }

    // the stream's, unless it's done receiving.
    const auto it = streams.find(stream);
    if (it == streams.end() || it->second.remote_closed) return;

    auto& s = it->second;
    s.unconsumed += n;
    if (s.unconsumed >= config.local.initial_window_size / 2)
    {
        encode_window_update(stream, static_cast<std::uint32_t>(s.unconsumed), output);
        s.recv_window += static_cast<std::int64_t>(s.unconsumed);
        s.unconsumed = 0;
    }
}

void session::goaway(stream_id last_stream, error_code code, std::string_view debug)
{
    last_stream  = std::min(last_stream, local_goaway.value_or(max_stream_id));
    local_goaway = last_stream;
    encode_goaway(last_stream, static_cast<std::uint32_t>(code), debug, output);
}

void session::ping(std::span<const std::byte, 8> opaque) { encode_ping(opaque, false, output); }

void session::take_output(std::vector<std::byte>& out)
{
    out.clear();
    out.swap(output);
    pending_replies = 0;
    pending_updates.clear();
}

std::int64_t session::send_window(stream_id stream) const noexcept
{
    if (stream == 0) return std::max<std::int64_t>(connection_send_window, 0);

    const auto it = streams.find(stream);
    if (it == streams.end() || it->second.local_closed) return 0;
    return std::max<std::int64_t>(std::min(it->second.send_window, connection_send_window), 0);
}

std::error_code session::stream_error(stream_id stream, error_code code, session_events* events, time_point now)
{
    if (auto err = glitch(now)) return err;

    const auto it    = streams.find(stream);
    const bool known = it != streams.end() && it->second.known;

    encode_rst_stream(stream, static_cast<std::uint32_t>(code), output);
    remember_reset(stream);
    if (it != streams.end()) forget(stream);

    if (known && events != nullptr) events->on_stream_reset(stream, make_error_code(code));
    return {};
}

std::error_code
session::data_error(stream_id stream, std::size_t length, error_code code, session_events& events, time_point now)
{
    // the stream won't consume the frame: what it took of the connection's window is given back at once, as for a
    // stream that's gone (once it's forgotten, so only the connection's).
    auto err = stream_error(stream, code, &events, now);
    if (!err) give_back(stream, length);
    return err;
}

std::error_code session::glitch(time_point now)
{
    if (!config.glitches.take(now)) return connection_error(error_code::enhance_your_calm, "too many errors");
    return {};
}

std::error_code session::connection_error(error_code code, std::string_view debug)
{
    if (!connection_failed)
    {
        // a GOAWAY this side has sent mustn't be raised (RFC 9113, section 6.8).
        encode_goaway(std::min(highest_peer_stream, local_goaway.value_or(max_stream_id)),
                      static_cast<std::uint32_t>(code),
                      debug,
                      output);
        connection_failed = true;
        failure           = make_error_code(code);
    }
    return failure;
}

std::error_code session::count_reply()
{
    if (++pending_replies > config.max_pending_replies)
        return connection_error(error_code::enhance_your_calm, "too many pings or settings");
    return {};
}

void session::close_local(stream_id stream)
{
    const auto it = streams.find(stream);
    if (it == streams.end()) return;

    it->second.local_closed = true;
    if (it->second.remote_closed) forget(stream);
}

void session::close_remote(stream_id stream)
{
    // the server's ended its response: an update waiting for it is taken back (it would only be ignored).
    take_back_update(stream);

    const auto it = streams.find(stream);
    if (it == streams.end()) return;

    it->second.remote_closed = true;
    if (it->second.local_closed) forget(stream);
}

void session::forget(stream_id stream)
{
    take_back_update(stream);
    if (streams.erase(stream) == 0) return;

    if (peer_initiated(stream)) --open_peer_streams;
    else --open_local_streams;
}

void session::remember_reset(stream_id stream)
{
    recently_reset.push_back(stream);
    if (recently_reset.size() > max_remembered_resets) recently_reset.pop_front();
}

bool session::was_reset(stream_id stream) const noexcept { return std::ranges::contains(recently_reset, stream); }

bool session::ignores(stream_id stream) const noexcept
{
    if (was_reset(stream)) return true;
    return side == role::server && peer_initiated(stream) && local_goaway.has_value() && stream > *local_goaway
        && closed(stream);
}

bool session::closed(stream_id stream) const noexcept
{
    if (streams.contains(stream)) return false;
    return peer_initiated(stream) ? stream <= highest_peer_stream : stream < next_local_stream;
}

bool session::peer_initiated(stream_id stream) const noexcept
{
    // clients' streams are odd, servers' even.
    return ((stream % 2) == 1) == (side == role::server);
}

}
