#include "instrument/opentelemetry/tracecontext.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>

#include "http/headers.hpp"
#include "instrument/opentelemetry/protocol.hpp"

namespace
{

using namespace std::string_view_literals;

using net::instrument::opentelemetry::span;
using net::instrument::opentelemetry::span_flags;
using net::instrument::opentelemetry::span_id_t;
using net::instrument::opentelemetry::span_kind;

// traceparent is "{version}-{trace-id}-{parent-id}-{flags}", in lowercase hex: e.g.
// "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01". Later versions may add fields after these.
constexpr std::size_t version_length     = 2;
constexpr std::size_t trace_id_offset    = version_length + 1;
constexpr std::size_t trace_id_length    = 32;
constexpr std::size_t parent_id_offset   = trace_id_offset + trace_id_length + 1;
constexpr std::size_t parent_id_length   = 16;
constexpr std::size_t flags_offset       = parent_id_offset + parent_id_length + 1;
constexpr std::size_t flags_length       = 2;
constexpr std::size_t traceparent_length = flags_offset + flags_length;

constexpr auto hex_digits = "0123456789abcdef"sv;

// decode_lowercase_hex decodes text (which must be twice out's size) into out, failing for anything but lowercase hex.
bool decode_lowercase_hex(std::string_view text, std::span<std::byte> out) noexcept
{
    if (text.size() != out.size() * 2) return false;

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        auto hi = hex_digits.find(text[i * 2]);
        auto lo = hex_digits.find(text[(i * 2) + 1]);
        if (hi == std::string_view::npos || lo == std::string_view::npos) return false;

        out[i] = static_cast<std::byte>((hi << 4) | lo);
    }

    return true;
}

void append_lowercase_hex(std::string& out, std::span<const std::byte> data)
{
    for (auto b : data)
    {
        auto v = static_cast<std::uint8_t>(b);
        out += hex_digits[v >> 4];
        out += hex_digits[v & 0xf];
    }
}

bool all_zero(std::span<const std::byte> data) noexcept
{
    return std::ranges::all_of(data, [](std::byte b) { return b == std::byte{0}; });
}

// new_span_id returns a random (non-zero) span id.
span_id_t new_span_id()
{
    thread_local std::mt19937_64 random{std::random_device{}()};

    span_id_t id{};
    do
    {
        auto v = random();
        for (auto& b : id)
        {
            b = static_cast<std::byte>(v & 0xff);
            v >>= 8;
        }
    }
    while (all_zero(id));

    return id;
}

std::optional<span> parse_traceparent(std::string_view traceparent)
{
    // the version, of which ff is invalid. Later versions than 00 are parsed as 00, ignoring what follows its fields
    // (which must be separated from them by a '-').
    std::array<std::byte, 1> version{};
    if (traceparent.size() < traceparent_length) return std::nullopt;
    if (!decode_lowercase_hex(traceparent.substr(0, version_length), version)) return std::nullopt;
    if (version[0] == std::byte{0xff}) return std::nullopt;
    if (version[0] == std::byte{0} ? traceparent.size() != traceparent_length
                                   : traceparent.size() > traceparent_length && traceparent[traceparent_length] != '-')
    {
        return std::nullopt;
    }

    if (traceparent[trace_id_offset - 1] != '-' || traceparent[parent_id_offset - 1] != '-'
        || traceparent[flags_offset - 1] != '-')
    {
        return std::nullopt;
    }

    span s{};
    s.kind           = span_kind::server;
    s.parent_span_id = span_id_t{};

    std::array<std::byte, 1> flags{};
    if (!decode_lowercase_hex(traceparent.substr(trace_id_offset, trace_id_length), s.trace_id)
        || !decode_lowercase_hex(traceparent.substr(parent_id_offset, parent_id_length), *s.parent_span_id)
        || !decode_lowercase_hex(traceparent.substr(flags_offset, flags_length), flags))
    {
        return std::nullopt;
    }

    // all-zero ids are invalid.
    if (all_zero(s.trace_id) || all_zero(*s.parent_span_id)) return std::nullopt;

    s.flags   = static_cast<span_flags>(flags[0]);
    s.span_id = new_span_id();
    return s;
}

}

namespace net::instrument::opentelemetry::tracecontext
{

std::optional<span> parse(const http::headers& headers)
{
    // more than one traceparent is invalid.
    auto traceparents = headers.get_all("traceparent"sv);
    if (!traceparents.has_value() || std::distance(traceparents->begin(), traceparents->end()) != 1)
    {
        return std::nullopt;
    }

    auto s = parse_traceparent(*traceparents->begin());
    if (!s.has_value()) return std::nullopt;

    // tracestate may be split across several field lines, which are combined (as a list).
    if (auto states = headers.get_all("tracestate"sv); states.has_value())
    {
        for (std::string_view state : *states)
        {
            if (!s->trace_state.empty()) s->trace_state += ',';
            s->trace_state += state;
        }
    }

    return s;
}

void inject(const span& span, http::headers& headers)
{
    // always version 00, which is the version this implements (whatever version the span was received with).
    std::string traceparent = "00-";
    traceparent.reserve(traceparent_length);

    append_lowercase_hex(traceparent, span.trace_id);
    traceparent += '-';
    append_lowercase_hex(traceparent, span.span_id);
    traceparent += '-';
    append_lowercase_hex(traceparent, std::array{static_cast<std::byte>(span.flags)});

    headers.set("traceparent"sv, traceparent);

    if (!span.trace_state.empty()) headers.set("tracestate"sv, span.trace_state);
}

}
