#include "instrument/opentelemetry/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception> // IWYU pragma: keep
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

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

using any_value      = net::instrument::opentelemetry::any_value;
using array_value    = net::instrument::opentelemetry::array_value;
using key_value_list = net::instrument::opentelemetry::key_value_list;
using key_value      = net::instrument::opentelemetry::key_value;

using json = nlohmann::json;

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
            .expect = R"({
  "stringValue": "foo"
})",
    },
        test_case{
            .name   = "bool",
            .input  = {.value = true},
            .expect = R"({
  "boolValue": true
})",
        },
        test_case{
            .name   = "int",
            .input  = {.value = 7},
            .expect = R"({
  "intValue": 7
})",
        },
        test_case{
            .name   = "double",
            .input  = {.value = 7.3},
            .expect = R"({
  "doubleValue": 7.3
})",
        },
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
            .expect = R"({
  "arrayValue": {
    "values": [
      {
        "stringValue": "foo"
      },
      {
        "intValue": 5
      },
      {
        "boolValue": false
      },
      {
        "arrayValue": {
          "values": [
            {
              "doubleValue": 7.3
            }
          ]
        }
      }
    ]
  }
})",
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
            .expect = R"({
  "kvlistValue": {
    "values": [
      {
        "key": "foo",
        "value": {
          "stringValue": "bar"
        }
      }
    ]
  }
})",
        },
        test_case{
            .name   = "bytes",
            .input  = {.value = "foo"_bytes},
            .expect = R"({
  "bytesValue": "Zm9v"
})",
        });

    json j = v.input;

    SECTION(v.name)
    {
        CHECK(j.dump(2) == v.expect);
    }
}

TEST_CASE("serialize non-finite doubles", "[instrument][opentelemetry][protocol][serialize]")
{
    // as proto3's JSON mapping does, rather than as null.
    CHECK(json(any_value{.value = std::numeric_limits<double>::quiet_NaN()}).dump() == R"({"doubleValue":"NaN"})");
    CHECK(json(any_value{.value = std::numeric_limits<double>::infinity()}).dump() == R"({"doubleValue":"Infinity"})");
    CHECK(json(any_value{.value = -std::numeric_limits<double>::infinity()}).dump()
          == R"({"doubleValue":"-Infinity"})");
    CHECK(json(any_value{.value = 1.5}).dump() == R"({"doubleValue":1.5})");
}

TEST_CASE("serialize a span's times since the Unix epoch", "[instrument][opentelemetry][protocol][serialize]")
{
    using namespace std::chrono;

    net::instrument::opentelemetry::span s{};
    s.start_time = sys_days{year{2026} / 1 / 1};
    s.end_time   = s.start_time + 1500ms;

    json j = s;
    CHECK(j["startTimeUnixNano"] == 1'767'225'600'000'000'000);
    CHECK(j["endTimeUnixNano"] == 1'767'225'601'500'000'000);

    // and its unset fields are zero, rather than whatever was in memory.
    CHECK(j["droppedAttributesCount"] == 0);
    CHECK(j["droppedEventsCount"] == 0);
    CHECK(j["droppedLinksCount"] == 0);
    CHECK(j["kind"] == 0);
    CHECK(j["traceId"] == "00000000000000000000000000000000");
}

TEST_CASE("serialize an exponential histogram's scale", "[instrument][opentelemetry][protocol][serialize]")
{
    net::instrument::opentelemetry::exponential_histogram::data_point point{};
    point.scale = -3;

    json j = net::instrument::opentelemetry::exponential_histogram{.data_points = {point}};
    CHECK(j["dataPoints"][0]["scale"] == -3);
}

TEST_CASE("deserialize partial success counts", "[instrument][opentelemetry][protocol][deserialize]")
{
    using net::instrument::opentelemetry::export_metrics_service_response;
    using net::instrument::opentelemetry::export_trace_service_response;

    // as a string (as proto3's JSON mapping writes int64s), a number, or absent.
    auto traces = json::parse(R"({"partialSuccess":{"rejectedSpans":"9007199254740993","errorMessage":"some"}})")
                      .get<export_trace_service_response>();
    REQUIRE(traces.partial_success.has_value());
    CHECK(traces.partial_success->rejected_spans == 9'007'199'254'740'993);
    CHECK(traces.partial_success->error_message == "some");

    auto metrics = json::parse(R"({"partialSuccess":{"rejectedDataPoints":3}})").get<export_metrics_service_response>();
    REQUIRE(metrics.partial_success.has_value());
    CHECK(metrics.partial_success->rejected_data_points == 3);
    CHECK(metrics.partial_success->error_message.empty());

    CHECK_FALSE(json::parse("{}").get<export_trace_service_response>().partial_success.has_value());

    CHECK_THROWS_AS(json::parse(R"({"partialSuccess":{"rejectedSpans":"lots"}})").get<export_trace_service_response>(),
                    std::invalid_argument);
}
