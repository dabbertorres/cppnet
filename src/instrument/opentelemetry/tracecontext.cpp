#include "instrument/opentelemetry/tracecontext.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "encoding/hex.hpp"
#include "http/headers.hpp"
#include "instrument/opentelemetry/protocol.hpp"

namespace net::instrument::opentelemetry::tracecontext
{

using namespace std::string_view_literals;

static constexpr auto version_length = 2u;

static constexpr auto trace_id_00_length  = 32u;
static constexpr auto parent_id_00_length = 16u;
static constexpr auto flags_00_length     = 2u;
static constexpr auto traceparent_00_length =
    version_length + 1 + trace_id_00_length + 1 + parent_id_00_length + 1 + flags_00_length;

std::optional<span> parse_version_00(std::string_view traceparent) noexcept;

std::optional<span> parse(const http::headers& headers) noexcept
{
    auto s = headers.get("traceparent"sv)
                 .and_then(
                     [](std::string_view tp) -> std::optional<span>
                     {
                         auto version_end = tp.find_first_of('-');
                         if (version_end == std::string_view::npos) return std::nullopt;

                         auto version_raw = tp.substr(0, version_end);
                         tp               = tp.substr(version_end + 1);

                         if (version_raw.length() != version_length) return std::nullopt;

                         auto version = encoding::hex::decode(version_raw[0], version_raw[1]);

                         switch (static_cast<std::uint8_t>(version))
                         {
                         case 0: return parse_version_00(tp);
                         default: return std::nullopt;
                         }
                     });

    if (!s.has_value()) return std::nullopt;

    s->kind        = span_kind::server;
    s->trace_state = headers.get("tracestate"sv).value_or(""sv);
    return s;
}

std::optional<span> parse_version_00(std::string_view traceparent) noexcept
{
    static constexpr auto expected_length = trace_id_00_length + 1 + parent_id_00_length + 1 + version_length;

    if (traceparent.length() != expected_length) return std::nullopt;

    if (traceparent[trace_id_00_length] != '-') return std::nullopt;
    if (traceparent[trace_id_00_length + 1 + parent_id_00_length] != '-') return std::nullopt;

    auto trace_id_raw  = traceparent.substr(0, trace_id_00_length);
    auto parent_id_raw = traceparent.substr(trace_id_00_length + 1, parent_id_00_length);
    auto flags_raw     = traceparent.substr(trace_id_00_length + 1 + parent_id_00_length + 1, flags_00_length);

    span s = {
        .version        = static_cast<std::byte>(0),
        .parent_span_id = std::make_optional<span_id_t>(),
        .flags          = static_cast<span_flags>(encoding::hex::decode(flags_raw[0], flags_raw[1])),
    };

    if (!encoding::hex::decode_to(trace_id_raw, s.trace_id)) return std::nullopt;
    if (!encoding::hex::decode_to(parent_id_raw, *s.parent_span_id)) return std::nullopt;

    return s;
}

void inject_version_00(const span& span, http::headers& headers) noexcept;

void inject(const span& span, http::headers& headers) noexcept
{
    switch (static_cast<std::uint8_t>(span.version))
    {
    case 0: inject_version_00(span, headers); break;
    default: break;
    }
}

void inject_version_00(const span& span, http::headers& headers) noexcept
{
    std::string traceparent(traceparent_00_length, '0');
    encoding::hex::encode_to(span.version, traceparent);
    traceparent[version_length] = '-';

    encoding::hex::encode_to(span.trace_id, traceparent, version_length + 1);
    traceparent[version_length + 1 + traceparent_00_length] = '-';

    encoding::hex::encode_to(span.span_id, traceparent, version_length + 1 + traceparent_00_length + 1);
    traceparent[version_length + 1 + traceparent_00_length + 1 + parent_id_00_length] = '-';

    encoding::hex::encode_to(static_cast<std::byte>(span.flags),
                             traceparent,
                             version_length + 1 + traceparent_00_length + 1 + parent_id_00_length + 1);

    headers.set("traceparent"sv, traceparent);
    if (!span.trace_state.empty()) headers.set("tracestate"sv, span.trace_state);
}

}
