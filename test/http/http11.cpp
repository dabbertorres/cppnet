#include "http/http11.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "io/buffered_reader.hpp"
#include "io/string_reader.hpp"
#include "string_makers.hpp"
#include "url.hpp"
#include "util/string_util.hpp"

using namespace std::string_view_literals;

TEST_CASE("just a request line", "[http][1.1][request_decode]")
{
    net::io::string_reader<char> content("GET /some/resource HTTP/1.1\r\n\r\n");

    auto task   = net::http::http11::request_decode(std::make_unique<net::io::buffered_reader>(&content));
    auto result = std::move(task).operator co_await().await_resume();

    REQUIRE(result.has_value());

    auto route = net::url::parse("/some/resource"sv);
    REQUIRE(route.has_value());
    const auto expected_route = route.to_value();

    auto request = result.to_value();

    SECTION("expected result")
    {
        CHECK(request.method == net::http::request_method::GET);
        CHECK(request.version == net::http::protocol_version{.major = 1, .minor = 1});
        CHECK(request.uri == expected_route);
        CHECK(request.headers.empty());
    }
}

TEST_CASE("with headers", "[http][1.1][request_decode]")
{
    net::io::string_reader<char> content("GET /some/resource HTTP/1.1\r\n"
                                         "Accept: application/json\r\n"
                                         "X-Hello: hello world\r\n"
                                         "\r\n");

    auto task   = net::http::http11::request_decode(std::make_unique<net::io::buffered_reader>(&content));
    auto result = std::move(task).operator co_await().await_resume();

    REQUIRE(result.has_value());

    auto route = net::url::parse("/some/resource"sv);
    REQUIRE(route.has_value());
    const auto expected_route = route.to_value();

    const net::http::headers expect_headers{
        { "Accept", {"application/json"}},
        {"X-Hello",      {"hello world"}},
    };

    auto request = result.to_value();

    SECTION("expected result")
    {
        CHECK(request.method == net::http::request_method::GET);
        CHECK(request.version == net::http::protocol_version{.major = 1, .minor = 1});
        CHECK(request.uri == expected_route);
        CHECK(request.headers == expect_headers);
    }
}

TEST_CASE("with headers and body", "[http][1.1][request_decode]")
{
    net::io::string_reader<char> content("POST /some/resource HTTP/1.1\r\n"
                                         "Accept: application/json\r\n"
                                         "X-Hello: hello world\r\n"
                                         "Content-Type: text/plain\r\n"
                                         "Content-Length: 18\r\n"
                                         "\r\n"
                                         "this is a request\n");
    net::io::buffered_reader     buf_reader(&content);

    auto task   = net::http::http11::request_decode(std::make_unique<net::io::buffered_reader>(&content));
    auto result = std::move(task).operator co_await().await_resume();

    REQUIRE(result.has_value());

    auto route = net::url::parse("/some/resource"sv);
    REQUIRE(route.has_value());
    const auto expected_route = route.to_value();

    const net::http::headers expect_headers{
        {        "Accept", {"application/json"}},
        {       "X-Hello",      {"hello world"}},
        {  "Content-Type",       {"text/plain"}},
        {"Content-Length",               {"18"}},
    };

    auto request = result.to_value();

    std::string body(64, 0);

    auto read_task   = request.body->read(body);
    auto read_result = std::move(read_task).operator co_await().await_resume();
    CHECK_FALSE(read_result.err);
    CHECK(read_result.count == 18);

    body = body.substr(0, read_result.count);
    body = net::util::trim_string(body);

    SECTION("expected result")
    {
        CHECK(request.method == net::http::request_method::POST);
        CHECK(request.version == net::http::protocol_version{.major = 1, .minor = 1});
        CHECK(request.uri == expected_route);
        CHECK(request.headers == expect_headers);
        CHECK(body == "this is a request"sv);
    }
}

TEST_CASE("with headers and multi-line body", "[http][1.1][request_decode]")
{
    net::io::string_reader<char> content("POST /some/resource HTTP/1.1\r\n"
                                         "Accept: application/json\r\n"
                                         "X-Hello: hello world\r\n"
                                         "Content-Type: text/plain\r\n"
                                         "Content-Length: 38\r\n"
                                         "\r\n"
                                         "this is a request\n"
                                         "with\n"
                                         "multiple\n"
                                         "lines\n");
    net::io::buffered_reader     buf_reader(&content);

    auto task   = net::http::http11::request_decode(std::make_unique<net::io::buffered_reader>(&content));
    auto result = std::move(task).operator co_await().await_resume();

    REQUIRE(result.has_value());

    auto route = net::url::parse("/some/resource"sv);
    REQUIRE(route.has_value());
    const auto expected_route = route.to_value();

    const net::http::headers expect_headers{
        {        "Accept", {"application/json"}},
        {       "X-Hello",      {"hello world"}},
        {  "Content-Type",       {"text/plain"}},
        {"Content-Length",               {"38"}},
    };

    auto request = result.to_value();

    std::string body(64, 0);

    auto read_task   = request.body->read(body);
    auto read_result = std::move(read_task).operator co_await().await_resume();
    CHECK_FALSE(read_result.err);
    CHECK(read_result.count == 38);

    body = body.substr(0, read_result.count);
    body = net::util::trim_string(body);

    SECTION("expected result")
    {
        CHECK(request.method == net::http::request_method::POST);
        CHECK(request.version == net::http::protocol_version{.major = 1, .minor = 1});
        CHECK(request.uri == expected_route);
        CHECK(request.headers == expect_headers);
        CHECK(body
              == "this is a request\n"
                 "with\n"
                 "multiple\n"
                 "lines");
    }
}

TEST_CASE("chunked", "[http][1.1][request_decode]")
{
    // TODO
}
