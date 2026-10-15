#include "instrument/opentelemetry/tracecontext.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <span>
#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "encoding/hex.hpp"
#include "http/headers.hpp"
#include "instrument/opentelemetry/protocol.hpp"

using namespace std::string_view_literals;

using net::http::headers;
using net::instrument::opentelemetry::span;
using net::instrument::opentelemetry::span_flags;
using net::instrument::opentelemetry::span_id_t;
using net::instrument::opentelemetry::span_kind;
namespace tracecontext = net::instrument::opentelemetry::tracecontext;

namespace
{

constexpr auto valid = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"sv;

std::string lower_hex(std::span<const std::byte> data)
{
    std::string out = net::encoding::hex::encode(data);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}

TEST_CASE("tracecontext parses a traceparent", "[instrument][opentelemetry][tracecontext]")
{
    headers h;
    h.set("traceparent"sv, valid);

    auto s = tracecontext::parse(h);
    REQUIRE(s.has_value());

    CHECK(lower_hex(s->trace_id) == "4bf92f3577b34da6a3ce929d0e0e4736");
    REQUIRE(s->parent_span_id.has_value());
    CHECK(lower_hex(*s->parent_span_id) == "00f067aa0ba902b7");
    CHECK(s->flags == span_flags::sampled);
    CHECK(s->kind == span_kind::server);
    CHECK(s->trace_state.empty());

    // the span (this service's) has its own id.
    CHECK(s->span_id != span_id_t{});
    CHECK(s->span_id != *s->parent_span_id);
}

TEST_CASE("tracecontext combines tracestates", "[instrument][opentelemetry][tracecontext]")
{
    headers h;
    h.set("traceparent"sv, valid);
    h.add("tracestate"sv, "a=1,b=2"sv);
    h.add("tracestate"sv, "c=3"sv);

    auto s = tracecontext::parse(h);
    REQUIRE(s.has_value());
    CHECK(s->trace_state == "a=1,b=2,c=3");
}

TEST_CASE("tracecontext parses later versions", "[instrument][opentelemetry][tracecontext]")
{
    // as version 00, ignoring the fields after its own.
    for (auto traceparent : {"01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"sv,
                             "cc-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-what-comes-next"sv})
    {
        headers h;
        h.set("traceparent"sv, traceparent);

        auto s = tracecontext::parse(h);
        REQUIRE(s.has_value());
        CHECK(lower_hex(s->trace_id) == "4bf92f3577b34da6a3ce929d0e0e4736");
    }
}

TEST_CASE("tracecontext ignores invalid traceparents", "[instrument][opentelemetry][tracecontext]")
{
    auto traceparent = GENERATE(
        // uppercase hex.
        "00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01"sv,
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00F067AA0BA902B7-01"sv,
        // not hex.
        "00-4bf92f3577b34da6a3ce929d0e0e473g-00f067aa0ba902b7-01"sv,
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-zz"sv,
        "g0-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"sv,
        // all-zero ids.
        "00-00000000000000000000000000000000-00f067aa0ba902b7-01"sv,
        "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01"sv,
        // version ff.
        "ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"sv,
        // version 00 has nothing after its fields.
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-extra"sv,
        // later versions separate what's after them with '-'.
        "01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01extra"sv,
        // the wrong lengths or separators.
        "00-4bf92f3577b34da6a3ce929d0e0e473-00f067aa0ba902b7-01"sv,
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-1"sv,
        "00_4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"sv,
        "00-4bf92f3577b34da6a3ce929d0e0e4736_00f067aa0ba902b7-01"sv,
        ""sv);

    CAPTURE(traceparent);

    headers h;
    h.set("traceparent"sv, traceparent);
    h.set("tracestate"sv, "a=1"sv);

    CHECK_FALSE(tracecontext::parse(h).has_value());
}

TEST_CASE("tracecontext ignores more than one traceparent", "[instrument][opentelemetry][tracecontext]")
{
    headers h;
    h.add("traceparent"sv, valid);
    h.add("traceparent"sv, valid);

    CHECK_FALSE(tracecontext::parse(h).has_value());
    CHECK_FALSE(tracecontext::parse(headers{}).has_value());
}

TEST_CASE("tracecontext injects a span", "[instrument][opentelemetry][tracecontext]")
{
    headers incoming;
    incoming.set("traceparent"sv, valid);
    incoming.set("tracestate"sv, "a=1"sv);

    auto s = tracecontext::parse(incoming);
    REQUIRE(s.has_value());

    headers outgoing;
    tracecontext::inject(*s, outgoing);

    // the same trace, with this span as the parent: in lowercase.
    CHECK(outgoing.get("traceparent"sv) == "00-4bf92f3577b34da6a3ce929d0e0e4736-" + lower_hex(s->span_id) + "-01");
    CHECK(outgoing.get("tracestate"sv) == "a=1");

    // which parses back.
    auto next = tracecontext::parse(outgoing);
    REQUIRE(next.has_value());
    CHECK(next->trace_id == s->trace_id);
    CHECK(next->parent_span_id == s->span_id);
}

TEST_CASE("tracecontext injects version 00", "[instrument][opentelemetry][tracecontext]")
{
    headers incoming;
    incoming.set("traceparent"sv, "01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00-extra"sv);

    auto s = tracecontext::parse(incoming);
    REQUIRE(s.has_value());

    headers outgoing;
    tracecontext::inject(*s, outgoing);

    auto traceparent = outgoing.get("traceparent"sv);
    REQUIRE(traceparent.has_value());
    CHECK(traceparent->starts_with("00-"));
    CHECK(traceparent->ends_with("-00"));
    CHECK(traceparent->size() == valid.size());
}
