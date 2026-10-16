#include "instrument/opentelemetry/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace
{

std::vector<std::byte> operator""_bytes(const char* str, std::size_t length)
{
    std::vector<std::byte> out;
    out.resize(length);

    for (auto i = 0u; i < length; ++i)
    {
        out[i] = static_cast<std::byte>(str[i]);
    }

    return out;
}

}

using net::instrument::opentelemetry::any_value;
using net::instrument::opentelemetry::array_value;
using net::instrument::opentelemetry::key_value_list;

using net::instrument::opentelemetry::from_json;
using net::instrument::opentelemetry::to_json;

TEST_CASE("serialize any_value", "[instrument][opentelemetry][protocol][serialize]")
{
    struct test_case
    {
        const char*      name;
        any_value        input;
        std::string_view expect;
    };

    auto v = GENERATE(
        test_case{
            .name   = "string",
            .input  = {.value = "foo"},
            .expect = R"({"stringValue":"foo"})"
    },
        test_case{.name = "bool", .input = {.value = true}, .expect = R"({"boolValue":true})"},
        test_case{.name = "int", .input = {.value = 7}, .expect = R"({"intValue":7})"},
        test_case{.name = "double", .input = {.value = 7.3}, .expect = R"({"doubleValue":7.3})"},
        test_case{
            .name = "array",
            .input =
                {
                    .value =
                        array_value{
                            .values =
                                {
                                    {"foo"},
                                    {5},
                                    {false},
                                    {array_value{
                                        .values = {{.value = 7.3}},
                                    }},
                                },
                        },
                },
            .expect =
                R"({"arrayValue":{"values":[{"stringValue":"foo"},{"intValue":5},{"boolValue":false},{"arrayValue":{"values":[{"doubleValue":7.3}]}}]}})",
        },
        test_case{
            .name = "key value list",
            .input =
                {
                    .value =
                        key_value_list{
                            .values =
                                {
                                    {
                                        .key   = "foo",
                                        .value = {.value = "bar"},
                                    },
                                },
                        },
                },
            .expect = R"({"kvlistValue":{"values":[{"key":"foo","value":{"stringValue":"bar"}}]}})",
        },
        test_case{.name = "bytes", .input = {.value = "foo"_bytes}, .expect = R"({"bytesValue":"Zm9v"})"});

    SECTION(v.name)
    {
        CHECK(to_json(v.input) == v.expect);
    }
}

TEST_CASE("serialize non-finite doubles", "[instrument][opentelemetry][protocol][serialize]")
{
    // as proto3's JSON mapping does, rather than as null.
    CHECK(to_json(any_value{.value = std::numeric_limits<double>::quiet_NaN()}) == R"({"doubleValue":"NaN"})");
    CHECK(to_json(any_value{.value = std::numeric_limits<double>::infinity()}) == R"({"doubleValue":"Infinity"})");
    CHECK(to_json(any_value{.value = -std::numeric_limits<double>::infinity()}) == R"({"doubleValue":"-Infinity"})");
    CHECK(to_json(any_value{.value = 1.5}) == R"({"doubleValue":1.5})");
}

TEST_CASE("serialize a span", "[instrument][opentelemetry][protocol][serialize]")
{
    using namespace std::chrono_literals;

    net::instrument::opentelemetry::span s{};
    s.start_time = std::chrono::sys_days{std::chrono::year{2026} / 1 / 1};
    s.end_time   = s.start_time + 1500ms;

    // its times are since the Unix epoch, and its unset fields are zero, rather than whatever was in memory (and
    // traceState, parentSpanId and flags are left out).
    CHECK(to_json(s)
          == R"({"traceId":"00000000000000000000000000000000","spanId":"0000000000000000","name":"","kind":0,)"
             R"("startTimeUnixNano":1767225600000000000,"endTimeUnixNano":1767225601500000000,"attributes":[],)"
             R"("droppedAttributesCount":0,"events":[],"droppedEventsCount":0,"links":[],"droppedLinksCount":0,)"
             R"("status":{"message":"","code":0}})");

    s.trace_state    = "a=b";
    s.parent_span_id = net::instrument::opentelemetry::span_id_t{std::byte{1}};
    s.flags          = net::instrument::opentelemetry::span_flags::sampled;

    const auto out = to_json(s);
    CHECK(out.contains(R"("traceState":"a=b")"));
    CHECK(out.contains(R"("parentSpanId":"0100000000000000")"));
    CHECK(out.contains(R"("flags":1)"));
}

TEST_CASE("serialize a metric's data", "[instrument][opentelemetry][protocol][serialize]")
{
    using net::instrument::opentelemetry::exponential_histogram;
    using net::instrument::opentelemetry::metric;

    exponential_histogram::data_point point{};
    point.scale = -3;

    metric m{};
    m.name = "m";
    m.data = exponential_histogram{.data_points = {point}, .aggregation_temporality = {}};

    // as a field named for its type.
    const auto out = to_json(m);
    CHECK(out.contains(R"("exponentialHistogram":{"dataPoints":[{)"));
    CHECK(out.contains(R"("scale":-3)"));
    CHECK_FALSE(out.contains(R"("gauge")"));
}

TEST_CASE("deserialize partial success counts", "[instrument][opentelemetry][protocol][deserialize]")
{
    using net::instrument::opentelemetry::export_metrics_service_response;
    using net::instrument::opentelemetry::export_trace_service_response;

    // as a string (as proto3's JSON mapping writes int64s), a number, or absent.
    auto traces = from_json<export_trace_service_response>(
        R"({"partialSuccess":{"rejectedSpans":"9007199254740993","errorMessage":"some"}})");
    REQUIRE(traces.has_value());
    REQUIRE(traces->partial_success.has_value());
    CHECK(traces->partial_success->rejected_spans == 9'007'199'254'740'993);
    CHECK(traces->partial_success->error_message == "some");

    auto metrics = from_json<export_metrics_service_response>(R"({"partialSuccess":{"rejectedDataPoints":3}})");
    REQUIRE(metrics.has_value());
    REQUIRE(metrics->partial_success.has_value());
    CHECK(metrics->partial_success->rejected_data_points == 3);
    CHECK(metrics->partial_success->error_message.empty());

    // fields it doesn't know are skipped.
    auto empty = from_json<export_trace_service_response>(R"({"other":[1,2]})");
    REQUIRE(empty.has_value());
    CHECK_FALSE(empty->partial_success.has_value());

    CHECK(from_json<export_trace_service_response>(R"({"partialSuccess":{"rejectedSpans":"lots"}})").error()
          == std::errc::invalid_argument);
    CHECK(from_json<export_trace_service_response>("not json").error() == std::errc::invalid_argument);
}
