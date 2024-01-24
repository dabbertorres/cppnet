#include "instrument/opentelemetry/protocol.hpp"

#include <cstddef>
#include <cstring>
#include <exception> // IWYU pragma: keep
#include <string_view>
#include <vector>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

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

using any_value      = net::instrument::opentelemetry::any_value;
using any_value_t    = net::instrument::opentelemetry::any_value_t;
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
