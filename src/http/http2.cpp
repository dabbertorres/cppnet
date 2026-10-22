#include "http/http2.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

class category_impl final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "http2"; }

    [[nodiscard]] std::string message(int value) const override
    {
        using enum net::http::http2::error_code;
        switch (static_cast<net::http::http2::error_code>(value))
        {
        case no_error: return "no error";
        case protocol_error: return "protocol error";
        case internal_error: return "internal error";
        case flow_control_error: return "flow-control error";
        case settings_timeout: return "settings timeout";
        case stream_closed: return "stream closed";
        case frame_size_error: return "frame size error";
        case refused_stream: return "refused stream";
        case cancel: return "cancel";
        case compression_error: return "compression error";
        case connect_error: return "connect error";
        case enhance_your_calm: return "enhance your calm";
        case inadequate_security: return "inadequate security";
        case http_1_1_required: return "HTTP/1.1 required";
        }
        return "unknown HTTP/2 error " + std::to_string(static_cast<std::uint32_t>(value));
    }
};

const category_impl the_category;

// put appends value's lowest n octets to out, most significant first (as HTTP/2 is big-endian).
void put(std::uint32_t value, int n, std::vector<std::byte>& out)
{
    for (int i = n - 1; i >= 0; --i) out.push_back(static_cast<std::byte>(value >> (i * 8)));
}

void put_header(std::uint32_t                length,
                net::http::http2::frame_type type,
                std::uint8_t                 frame_flags,
                net::http::http2::stream_id  stream,
                std::vector<std::byte>&      out)
{
    net::http::http2::encode_frame_header(
        {.length = length, .type = static_cast<std::uint8_t>(type), .flags = frame_flags, .stream = stream},
        out);
}

}

namespace net::http::http2
{

const std::error_category& category() noexcept { return the_category; }

std::error_code make_error_code(error_code err) noexcept { return {static_cast<int>(err), the_category}; }

std::uint32_t wire_code(std::error_code err) noexcept
{
    if (err.category() == the_category) return static_cast<std::uint32_t>(err.value());
    return static_cast<std::uint32_t>(error_code::internal_error);
}

frame_header decode_frame_header(std::span<const std::byte, frame_header_size> in) noexcept
{
    // the length is 24 bits: loaded as the first 4 octets, the type's octet shifted out.
    std::uint32_t first = 0;
    std::memcpy(&first, in.data(), sizeof(first));
    std::uint32_t stream = 0;
    std::memcpy(&stream, in.data() + 5, sizeof(stream));
    if constexpr (std::endian::native == std::endian::little)
    {
        first  = std::byteswap(first);
        stream = std::byteswap(stream);
    }

    return {
        .length = first >> 8U,
        .type   = std::to_integer<std::uint8_t>(in[3]),
        .flags  = std::to_integer<std::uint8_t>(in[4]),
        .stream = stream & max_stream_id,
    };
}

void encode_frame_header(const frame_header& header, std::vector<std::byte>& out)
{
    put(header.length, 3, out);
    out.push_back(static_cast<std::byte>(header.type));
    out.push_back(static_cast<std::byte>(header.flags));
    put(header.stream & max_stream_id, 4, out);
}

void encode_settings(const settings& values, const settings& defaults, std::vector<std::byte>& out)
{
    std::vector<std::pair<setting_id, std::uint32_t>> changed;
    const auto add = [&](setting_id id, std::uint32_t value, std::uint32_t initial)
    {
        if (value != initial) changed.emplace_back(id, value);
    };

    add(setting_id::header_table_size, values.header_table_size, defaults.header_table_size);
    add(setting_id::enable_push, values.enable_push ? 1 : 0, defaults.enable_push ? 1 : 0);
    add(setting_id::max_concurrent_streams, values.max_concurrent_streams, defaults.max_concurrent_streams);
    add(setting_id::initial_window_size, values.initial_window_size, defaults.initial_window_size);
    add(setting_id::max_frame_size, values.max_frame_size, defaults.max_frame_size);
    add(setting_id::max_header_list_size, values.max_header_list_size, defaults.max_header_list_size);
    add(setting_id::no_rfc7540_priorities,
        values.no_rfc7540_priorities ? 1 : 0,
        defaults.no_rfc7540_priorities ? 1 : 0);

    put_header(static_cast<std::uint32_t>(changed.size() * 6), frame_type::settings, 0, 0, out);
    for (const auto& [id, value] : changed)
    {
        put(static_cast<std::uint32_t>(id), 2, out);
        put(value, 4, out);
    }
}

void encode_settings_ack(std::vector<std::byte>& out) { put_header(0, frame_type::settings, flags::ack, 0, out); }

void encode_ping(std::span<const std::byte, 8> opaque, bool ack, std::vector<std::byte>& out)
{
    put_header(8, frame_type::ping, ack ? flags::ack : 0, 0, out);
    out.insert(out.end(), opaque.begin(), opaque.end());
}

void encode_goaway(stream_id last_stream, std::uint32_t code, std::string_view debug, std::vector<std::byte>& out)
{
    put_header(static_cast<std::uint32_t>(8 + debug.size()), frame_type::goaway, 0, 0, out);
    put(last_stream & max_stream_id, 4, out);
    put(code, 4, out);
    const auto bytes = std::as_bytes(std::span{debug});
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void encode_rst_stream(stream_id stream, std::uint32_t code, std::vector<std::byte>& out)
{
    put_header(4, frame_type::rst_stream, 0, stream, out);
    put(code, 4, out);
}

void encode_window_update(stream_id stream, std::uint32_t increment, std::vector<std::byte>& out)
{
    put_header(4, frame_type::window_update, 0, stream, out);
    put(increment & max_stream_id, 4, out);
}

void encode_data(stream_id stream, std::span<const std::byte> data, bool end_stream, std::vector<std::byte>& out)
{
    put_header(static_cast<std::uint32_t>(data.size()),
               frame_type::data,
               end_stream ? flags::end_stream : 0,
               stream,
               out);
    out.insert(out.end(), data.begin(), data.end());
}

void encode_priority_update(stream_id prioritized, std::string_view value, std::vector<std::byte>& out)
{
    put_header(static_cast<std::uint32_t>(4 + value.size()), frame_type::priority_update, 0, 0, out);
    put(prioritized & max_stream_id, 4, out);
    const auto bytes = std::as_bytes(std::span{value});
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void encode_origin(std::span<const std::string> origins, std::vector<std::byte>& out)
{
    std::size_t length = 0;
    for (const auto& o : origins) length += 2 + o.size();

    put_header(static_cast<std::uint32_t>(length), frame_type::origin, 0, 0, out);
    for (const auto& o : origins)
    {
        put(static_cast<std::uint32_t>(o.size()), 2, out);
        const auto bytes = std::as_bytes(std::span{o});
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
}

void encode_headers(stream_id                  stream,
                    std::span<const std::byte> block,
                    bool                       end_stream,
                    std::uint32_t              max_frame_size,
                    std::vector<std::byte>&    out)
{
    auto type        = frame_type::headers;
    auto frame_flags = end_stream ? flags::end_stream : std::uint8_t{0};

    do
    {
        const auto fragment = block.first(std::min<std::size_t>(block.size(), max_frame_size));
        block               = block.subspan(fragment.size());
        if (block.empty()) frame_flags |= flags::end_headers;

        put_header(static_cast<std::uint32_t>(fragment.size()), type, frame_flags, stream, out);
        out.insert(out.end(), fragment.begin(), fragment.end());

        type        = frame_type::continuation;
        frame_flags = 0;
    }
    while (!block.empty());
}

}
