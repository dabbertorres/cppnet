#include "http/headers.hpp"

#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::string_view_literals;

TEST_CASE("list-based fields are split on commas outside quotes", "[http][headers][get_list]")
{
    net::http::headers fields;
    fields.add("Accept"sv, "text/html, application/json ,,"sv);
    fields.add("Accept"sv, R"(text/plain; note="a, \"b\", c", */*)"sv);

    CHECK(fields.get_list("Accept"sv)
          == std::vector{"text/html"sv, "application/json"sv, R"(text/plain; note="a, \"b\", c")"sv, "*/*"sv});

    CHECK(fields.get_list("Missing"sv).empty());
}

TEST_CASE("Content-Length must be digits, and agree with itself", "[http][headers][content_length]")
{
    const auto length = [](std::initializer_list<std::string_view> values)
    {
        net::http::headers fields;
        for (auto value : values) fields.add("Content-Length"sv, value);
        return fields.get_content_length();
    };

    CHECK(length({"5"}) == 5);
    CHECK(length({"5, 5", "5"}) == 5);
    CHECK(length({}) == std::nullopt);
    CHECK(length({"5", "6"}) == std::nullopt);
    CHECK(length({"5x"}) == std::nullopt);
    CHECK(length({"-5"}) == std::nullopt);
    CHECK(length({""}) == std::nullopt);
    CHECK(length({"99999999999999999999999"}) == std::nullopt);
}

TEST_CASE("is_chunked only if chunked is last", "[http][headers][is_chunked]")
{
    const auto chunked = [](std::string_view value)
    {
        net::http::headers fields;
        fields.add("Transfer-Encoding"sv, value);
        return fields.is_chunked();
    };

    CHECK(chunked("chunked"));
    CHECK(chunked("gzip, Chunked"));
    CHECK_FALSE(chunked("chunked, gzip"));
    CHECK_FALSE(chunked("xchunked"));
}
