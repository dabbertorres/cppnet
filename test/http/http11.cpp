#include "http/http11.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/sync_wait.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/string_reader.hpp"
#include "io/string_writer.hpp"
#include "io/writer.hpp"
#include "url.hpp"
#include "util/string_util.hpp"

using namespace std::string_view_literals;

TEST_CASE("just a request line", "[http][1.1][request_decode]")
{
    net::io::string_reader<char> content("GET /some/resource HTTP/1.1\r\nHost: a\r\n\r\n");
    net::io::buffered_reader     buf_reader(&content);

    auto task   = net::http::http11::request_decode(&buf_reader);
    auto result = test::sync_wait(std::move(task));

    REQUIRE(result.has_value());

    const net::http::request_target expected_route{.path = "/some/resource"};

    auto& request = result.value();

    SECTION("expected result")
    {
        CHECK(request.method == net::http::request_method::GET);
        CHECK(request.version == net::http::protocol_version{.major = 1, .minor = 1});
        CHECK(request.target == expected_route);
        CHECK(request.headers
              == net::http::headers{
                  {"Host", {"a"}}
        });
    }
}

TEST_CASE("with headers", "[http][1.1][request_decode]")
{
    net::io::string_reader<char> content("GET /some/resource HTTP/1.1\r\n"
                                         "Host: a\r\n"
                                         "Accept: application/json\r\n"
                                         "X-Hello: hello world\r\n"
                                         "\r\n");
    net::io::buffered_reader     buf_reader(&content);

    auto task   = net::http::http11::request_decode(&buf_reader);
    auto result = test::sync_wait(std::move(task));

    REQUIRE(result.has_value());

    const net::http::request_target expected_route{.path = "/some/resource"};

    const net::http::headers expect_headers{
        {   "Host",                {"a"}},
        { "Accept", {"application/json"}},
        {"X-Hello",      {"hello world"}},
    };

    auto& request = result.value();

    SECTION("expected result")
    {
        CHECK(request.method == net::http::request_method::GET);
        CHECK(request.version == net::http::protocol_version{.major = 1, .minor = 1});
        CHECK(request.target == expected_route);
        CHECK(request.headers == expect_headers);
    }
}

TEST_CASE("with headers and body", "[http][1.1][request_decode]")
{
    net::io::string_reader<char> content("POST /some/resource HTTP/1.1\r\n"
                                         "Host: a\r\n"
                                         "Accept: application/json\r\n"
                                         "X-Hello: hello world\r\n"
                                         "Content-Type: text/plain\r\n"
                                         "Content-Length: 18\r\n"
                                         "\r\n"
                                         "this is a request\n");
    net::io::buffered_reader     buf_reader(&content);

    auto task   = net::http::http11::request_decode(&buf_reader);
    auto result = test::sync_wait(std::move(task));

    REQUIRE(result.has_value());

    const net::http::request_target expected_route{.path = "/some/resource"};

    const net::http::headers expect_headers{
        {          "Host",                {"a"}},
        {        "Accept", {"application/json"}},
        {       "X-Hello",      {"hello world"}},
        {  "Content-Type",       {"text/plain"}},
        {"Content-Length",               {"18"}},
    };

    auto& request = result.value();

    std::string body(64, 0);

    auto read_task   = request.body->read(body);
    auto read_result = test::sync_wait(std::move(read_task));
    CHECK_FALSE(read_result.err);
    CHECK(read_result.count == 18);

    body = body.substr(0, read_result.count);
    body = net::util::trim_string(body);

    SECTION("expected result")
    {
        CHECK(request.method == net::http::request_method::POST);
        CHECK(request.version == net::http::protocol_version{.major = 1, .minor = 1});
        CHECK(request.target == expected_route);
        CHECK(request.headers == expect_headers);
        CHECK(body == "this is a request"sv);
    }
}

TEST_CASE("with headers and multi-line body", "[http][1.1][request_decode]")
{
    net::io::string_reader<char> content("POST /some/resource HTTP/1.1\r\n"
                                         "Host: a\r\n"
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

    auto task   = net::http::http11::request_decode(&buf_reader);
    auto result = test::sync_wait(std::move(task));

    REQUIRE(result.has_value());

    const net::http::request_target expected_route{.path = "/some/resource"};

    const net::http::headers expect_headers{
        {          "Host",                {"a"}},
        {        "Accept", {"application/json"}},
        {       "X-Hello",      {"hello world"}},
        {  "Content-Type",       {"text/plain"}},
        {"Content-Length",               {"38"}},
    };

    auto& request = result.value();

    std::string body(64, 0);

    auto read_task   = request.body->read(body);
    auto read_result = test::sync_wait(std::move(read_task));
    CHECK_FALSE(read_result.err);
    CHECK(read_result.count == 38);

    body = body.substr(0, read_result.count);
    body = net::util::trim_string(body);

    SECTION("expected result")
    {
        CHECK(request.method == net::http::request_method::POST);
        CHECK(request.version == net::http::protocol_version{.major = 1, .minor = 1});
        CHECK(request.target == expected_route);
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
    net::io::string_reader<char> content("POST /some/resource HTTP/1.1\r\n"
                                         "Host: a\r\n"
                                         "Transfer-Encoding: chunked\r\n"
                                         "\r\n"
                                         "5\r\n"
                                         "hello\r\n"
                                         "0\r\n"
                                         "\r\n"
                                         "GET /next HTTP/1.1\r\n"
                                         "Host: a\r\n"
                                         "\r\n");
    net::io::buffered_reader     buf_reader(&content);

    auto result = test::sync_wait(net::http::http11::request_decode(&buf_reader));
    REQUIRE(result.has_value());

    std::string body(64, 0);

    auto read_result = test::sync_wait(result->body->read(body));
    REQUIRE_FALSE(read_result.err);
    CHECK(body.substr(0, read_result.count) == "hello"sv);

    // the body ends at the final chunk, rather than reading into the next request.
    read_result = test::sync_wait(result->body->read(body));
    CHECK(read_result.err == net::io::errc::end_of_stream);
    CHECK(read_result.count == 0);

    auto next = test::sync_wait(net::http::http11::request_decode(&buf_reader));
    REQUIRE(next.has_value());
    CHECK(next->method == net::http::request_method::GET);
    CHECK(next->target.path == "/next"sv);
}

TEST_CASE("next request follows the body", "[http][1.1][request_decode]")
{
    net::io::string_reader<char> content("POST /first HTTP/1.1\r\n"
                                         "Host: a\r\n"
                                         "Content-Length: 5\r\n"
                                         "\r\n"
                                         "hello"
                                         "GET /second HTTP/1.1\r\n"
                                         "Host: a\r\n"
                                         "\r\n");
    net::io::buffered_reader     buf_reader(&content);

    auto first = test::sync_wait(net::http::http11::request_decode(&buf_reader));
    REQUIRE(first.has_value());

    std::string body(64, 0);

    auto read_result = test::sync_wait(first->body->read(body));
    REQUIRE_FALSE(read_result.err);
    CHECK(body.substr(0, read_result.count) == "hello"sv);

    read_result = test::sync_wait(first->body->read(body));
    CHECK(read_result.err == net::io::errc::end_of_stream);

    auto second = test::sync_wait(net::http::http11::request_decode(&buf_reader));
    REQUIRE(second.has_value());
    CHECK(second->method == net::http::request_method::GET);
    CHECK(second->target.path == "/second"sv);
}

TEST_CASE("invalid percent-encoding in the target", "[http][1.1][request_decode]")
{
    // used to throw out of url::decode (which is noexcept), terminating the server. Now it's a malformed request.
    net::io::string_reader<char> content("GET /% HTTP/1.1\r\n"
                                         "Host: a\r\n"
                                         "\r\n");
    net::io::buffered_reader     buf_reader(&content);

    auto result = test::sync_wait(net::http::http11::request_decode(&buf_reader));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == std::errc::invalid_argument);
}

TEST_CASE("host comes from the target, if it has one", "[http][1.1][request_decode]")
{
    SECTION("origin-form")
    {
        net::io::string_reader<char> content("GET /path HTTP/1.1\r\nHost: example.com\r\n\r\n");
        net::io::buffered_reader     buf_reader(&content);

        auto result = test::sync_wait(net::http::http11::request_decode(&buf_reader));
        REQUIRE(result.has_value());
        CHECK(result->host == "example.com");
    }

    SECTION("absolute-form")
    {
        net::io::string_reader<char> content("GET http://target.example/path HTTP/1.1\r\nHost: header.example\r\n\r\n");
        net::io::buffered_reader     buf_reader(&content);

        auto result = test::sync_wait(net::http::http11::request_decode(&buf_reader));
        REQUIRE(result.has_value());
        CHECK(result->host == "target.example");
        CHECK(result->target.path == "/path");
    }
}

TEST_CASE("requests are encoded in origin-form, with a Host header", "[http][1.1][request_encode]")
{
    auto parsed = net::url::parse("http://example.com:8080/a b");
    REQUIRE(parsed.has_value());

    net::http::client_request req{
        .method  = net::http::request_method::GET,
        .version = {.major = 1, .minor = 1},
        .uri     = parsed.value(),
    };

    // nothing in the url can end the request line, and start a header.
    req.uri.path += "\r\nX-Injected: yes";
    req.uri.query["q"] = {"x&y=z"};

    net::io::string_writer<char> out;
    auto                         res = test::sync_wait(net::http::http11::request_encode(&out, req));
    REQUIRE(res.has_value());

    CHECK(out.build() == "GET /a%20b%0D%0AX-Injected:%20yes?q=x%26y%3Dz HTTP/1.1\r\nHost: example.com:8080\r\n\r\n");
}

namespace
{

// decode decodes one request from text, with the given limit on the size of its head.
net::http::request_decoder_result decode(std::string_view text, std::size_t max_header_bytes = 8'192)
{
    net::io::string_reader<char> content{text};
    net::io::buffered_reader     buf_reader{&content};

    return test::sync_wait(net::http::http11::request_decode(&buf_reader, max_header_bytes));
}

}

TEST_CASE("malformed requests are rejected", "[http][1.1][request_decode][strict]")
{
    struct test_case
    {
        std::string_view name;
        std::string_view request;
        std::errc        expect = std::errc::invalid_argument;
    };

    auto tc = GENERATE(
        // the request line
        test_case{.name = "lowercase version", .request = "GET / http/1.1\r\nHost: a\r\n\r\n"},
        test_case{.name = "version with junk", .request = "GET / HTTP/1.1x\r\nHost: a\r\n\r\n"},
        test_case{.name = "two-digit minor", .request = "GET / HTTP/1.10\r\nHost: a\r\n\r\n"},
        test_case{.name = "no minor", .request = "GET / HTTP/1\r\nHost: a\r\n\r\n"},
        test_case{.name = "trailing space", .request = "GET / HTTP/1.1 \r\nHost: a\r\n\r\n"},
        test_case{.name = "double space", .request = "GET  / HTTP/1.1\r\nHost: a\r\n\r\n"},
        test_case{.name = "method isn't a token", .request = "G(T / HTTP/1.1\r\nHost: a\r\n\r\n"},
        test_case{.name    = "unknown method",
                  .request = "BREW / HTTP/1.1\r\nHost: a\r\n\r\n",
                  .expect  = std::errc::not_supported},
        // field lines
        test_case{.name = "no colon", .request = "GET / HTTP/1.1\r\nHost: a\r\nX-Nothing\r\n\r\n"},
        test_case{.name = "empty name", .request = "GET / HTTP/1.1\r\nHost: a\r\n: x\r\n\r\n"},
        test_case{.name = "space in name", .request = "GET / HTTP/1.1\r\nHost: a\r\nX Y: z\r\n\r\n"},
        test_case{.name = "space before colon", .request = "GET / HTTP/1.1\r\nHost: a\r\nX-Y : z\r\n\r\n"},
        test_case{.name = "obsolete line folding", .request = "GET / HTTP/1.1\r\nHost: a\r\nX-Y: z\r\n  more\r\n\r\n"},
        test_case{.name = "NUL in value", .request = "GET / HTTP/1.1\r\nHost: a\r\nX-Y: a\0b\r\n\r\n"sv},
        test_case{.name = "bare LF in value", .request = "GET / HTTP/1.1\r\nHost: a\r\nX-Y: a\nX-Z: b\r\n\r\n"},
        test_case{.name = "bare CR in value", .request = "GET / HTTP/1.1\r\nHost: a\r\nX-Y: a\rb\r\n\r\n"},
        // Host
        test_case{.name = "no Host", .request = "GET / HTTP/1.1\r\n\r\n"},
        test_case{.name = "two Hosts", .request = "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n"},
        test_case{.name = "two Hosts in one line", .request = "GET / HTTP/1.1\r\nHost: a, b\r\n\r\n"},
        test_case{.name = "invalid Host", .request = "GET / HTTP/1.1\r\nHost: a/b\r\n\r\n"},
        test_case{.name = "invalid Host port", .request = "GET / HTTP/1.1\r\nHost: a:http\r\n\r\n"},
        // the body's length
        test_case{.name    = "Content-Length with junk",
                  .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5x\r\n\r\n"},
        test_case{.name    = "negative Content-Length",
                  .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n"},
        test_case{.name    = "signed Content-Length",
                  .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: +5\r\n\r\n"},
        test_case{.name = "empty Content-Length", .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length:\r\n\r\n"},
        test_case{.name    = "huge Content-Length",
                  .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999999\r\n\r\n"},
        test_case{.name    = "different Content-Lengths",
                  .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n"},
        test_case{.name    = "different Content-Lengths in one line",
                  .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 6\r\n\r\n"},
        test_case{.name    = "Transfer-Encoding and Content-Length",
                  .request = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n"},
        test_case{.name    = "chunked isn't last",
                  .request = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked, gzip\r\n\r\n"},
        test_case{.name = "no chunked", .request = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n"},
        test_case{.name    = "not quite chunked",
                  .request = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: xchunked\r\n\r\n"},
        test_case{.name = "chunked twice",
                  .request =
                      "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n"},
        test_case{.name    = "Transfer-Encoding in HTTP/1.0",
                  .request = "POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n"},
        test_case{.name    = "unsupported coding",
                  .request = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
                  .expect  = std::errc::not_supported});

    CAPTURE(tc.name);

    auto result = decode(tc.request);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == tc.expect);
}

TEST_CASE("well-formed requests are accepted", "[http][1.1][request_decode][strict]")
{
    SECTION("HTTP/1.0 doesn't need a Host")
    {
        auto result = decode("GET / HTTP/1.0\r\n\r\n");
        REQUIRE(result.has_value());
        CHECK(result->host.empty());
    }

    SECTION("the Host header's port isn't part of the host")
    {
        auto result = decode("GET / HTTP/1.1\r\nHost: Example.com:8080\r\n\r\n");
        REQUIRE(result.has_value());
        CHECK(result->host == "Example.com");
    }

    SECTION("an IPv6 Host")
    {
        auto result = decode("GET / HTTP/1.1\r\nHost: [::1]:8080\r\n\r\n");
        REQUIRE(result.has_value());
        CHECK(result->host == "::1");
    }

    SECTION("values keep their commas, and lose surrounding whitespace")
    {
        auto result = decode("GET / HTTP/1.1\r\nHost: a\r\nUser-Agent: \t Mozilla/5.0 (X11, Linux) \t\r\n\r\n");
        REQUIRE(result.has_value());
        CHECK(result->headers.get("User-Agent"sv) == "Mozilla/5.0 (X11, Linux)");
    }

    SECTION("repeated, equal Content-Lengths")
    {
        auto result = decode("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 5\r\nContent-Length: 5\r\n\r\nhello");
        REQUIRE(result.has_value());
    }

    SECTION("chunked, in any case")
    {
        auto result = decode("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: ChUnKeD\r\n\r\n0\r\n\r\n");
        REQUIRE(result.has_value());
    }
}

TEST_CASE("the head of a request is limited in size", "[http][1.1][request_decode][strict]")
{
    const std::string long_target = "/" + std::string(100, 'a');
    const std::string long_value(100, 'v');

    CHECK(decode("GET " + long_target + " HTTP/1.1\r\nHost: a\r\n\r\n", 64).error() == std::errc::value_too_large);
    CHECK(decode("GET / HTTP/1.1\r\nHost: a\r\nX: " + long_value + "\r\n\r\n", 64).error()
          == std::errc::value_too_large);

    // many small lines add up.
    std::string many = "GET / HTTP/1.1\r\nHost: a\r\n";
    for (int i = 0; i < 20; ++i) many += "X: y\r\n";
    CHECK(decode(many + "\r\n", 64).error() == std::errc::value_too_large);

    // a line without an end doesn't buffer forever.
    CHECK(decode("GET / HTTP/1.1\r\nHost: a\r\nX: " + std::string(10'000, 'v'), 64).error()
          == std::errc::value_too_large);

    CHECK(decode("GET / HTTP/1.1\r\nHost: a\r\n\r\n", 64).has_value());
}

TEST_CASE("a request has at most max_fields field lines", "[http][1.1][request_decode][strict]")
{
    // Host, then the rest.
    std::string head = "GET / HTTP/1.1\r\nHost: a\r\n";
    for (std::size_t i = 1; i < net::http::http11::max_fields; ++i) head += "X-" + std::to_string(i) + ": y\r\n";

    CHECK(decode(head + "\r\n").has_value());
    CHECK(decode(head + "X-Last: y\r\n\r\n").error() == std::errc::value_too_large);
}

TEST_CASE("status lines", "[http][1.1][response_encode]")
{
    struct test_case
    {
        std::uint16_t    code;
        std::string_view want;
    };

    // codes without a name are still sent, without a reason phrase.
    auto tc = GENERATE(test_case{.code = 200, .want = "HTTP/1.1 200 OK\r\n"},
                       test_case{.code = 505, .want = "HTTP/1.1 505 HTTP Version Not Supported\r\n"},
                       test_case{.code = 299, .want = "HTTP/1.1 299 \r\n"},
                       test_case{.code = 100, .want = "HTTP/1.1 100 Continue\r\n"},
                       test_case{.code = 599, .want = "HTTP/1.1 599 Network Connect Timeout Error\r\n"});
    CAPTURE(tc.code);

    net::io::string_writer<char> out;
    net::http::server_response   resp{
        .version     = {.major = 1, .minor = 1},
        .status_code = static_cast<net::http::status>(tc.code)
    };

    auto res = test::sync_wait(net::http::http11::response_encode(&out, resp));
    REQUIRE(res.has_value());
    CHECK(out.build().starts_with(tc.want));
}

TEST_CASE("status codes outside 100-599 aren't encoded", "[http][1.1][response_encode][strict]")
{
    auto code = GENERATE(std::uint16_t{0}, std::uint16_t{99}, std::uint16_t{600}, std::uint16_t{1000});
    CAPTURE(code);

    net::io::string_writer<char> out;
    net::http::server_response   resp{
        .version     = {.major = 1, .minor = 1},
        .status_code = static_cast<net::http::status>(code)
    };

    auto res = test::sync_wait(net::http::http11::response_encode(&out, resp));
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == std::errc::invalid_argument);
    CHECK(out.build().empty());
}

TEST_CASE("responses without content have no Content-Length, and no body", "[http][1.1][response_writer]")
{
    struct test_case
    {
        net::http::status status;
        bool              content_length;
    };

    // RFC 9110, section 8.6: 1xx and 204 responses mustn't have one, and a 304's would have to be the 200's.
    auto tc = GENERATE(test_case{.status = net::http::status::OK, .content_length = true},
                       test_case{.status = net::http::status::NOT_FOUND, .content_length = true},
                       test_case{.status = net::http::status::CONTINUE, .content_length = false},
                       test_case{.status = net::http::status::NO_CONTENT, .content_length = false},
                       test_case{.status = net::http::status::NOT_MODIFIED, .content_length = false});
    CAPTURE(static_cast<int>(tc.status));

    net::io::string_writer<char> out;
    net::http::server_response   resp{
        .version = {.major = 1, .minor = 1},
        .body    = &out
    };
    net::http::response_writer rw{&out, &resp, net::http::http11::response_encode};

    // e.g. a handler setting it itself, as well.
    rw.headers().set_content_length(3);

    auto* body = test::sync_wait(rw.send(tc.status, 3));
    auto  res  = test::sync_wait(net::io::write_all(*body, "abc"sv));

    CHECK(out.build().contains("Content-Length") == tc.content_length);
    CHECK(static_cast<bool>(res.err) == !tc.content_length);
    CHECK(out.build().ends_with("abc") == tc.content_length);
}

TEST_CASE("unsafe header fields aren't encoded", "[http][1.1][response_encode][strict]")
{
    auto [name, value] = GENERATE(std::pair{"X-Y"sv, "a\r\nSet-Cookie: injected"sv},
                                  std::pair{"X-Y"sv, "a\nb"sv},
                                  std::pair{"X-Y"sv, "a\0b"sv},
                                  std::pair{"X-Y: z\r\nX"sv, "a"sv},
                                  std::pair{"X Y"sv, "a"sv},
                                  std::pair{""sv, "a"sv});
    CAPTURE(name, value);

    net::io::string_writer<char> out;
    net::http::server_response   resp{
        .version     = {.major = 1, .minor = 1},
        .status_code = net::http::status::OK
    };
    resp.headers.set(name, value);

    auto res = test::sync_wait(net::http::http11::response_encode(&out, resp));
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == std::errc::invalid_argument);
}

namespace
{

// string_holder holds a string_reader for owning_buffered_reader, as a base class so that it's constructed first.
struct string_holder
{
    net::io::string_reader<char> content;
};

// owning_buffered_reader buffers a string_reader that it owns, as a response's body owns its connection.
class owning_buffered_reader final
    : private string_holder
    , public net::io::buffered_reader
{
public:
    explicit owning_buffered_reader(std::string_view text)
        : string_holder{.content = net::io::string_reader<char>{text}}
        , net::io::buffered_reader{&content}
    {}
};

net::http::response_decoder_result decode_response(std::string_view          text,
                                                   net::http::request_method method = net::http::request_method::GET)
{
    return test::sync_wait(net::http::http11::response_decode(std::make_unique<owning_buffered_reader>(text), method));
}

std::string read_body(net::io::reader& body)
{
    std::string out;
    std::string buf(64, '\0');

    while (true)
    {
        auto res = test::sync_wait(body.read(buf));
        out.append(buf.data(), res.count);
        if (res.err) return res.err == net::io::errc::end_of_stream ? out : out + " [" + res.err.message() + "]";
    }
}

}

TEST_CASE("response bodies", "[http][1.1][response_decode]")
{
    using net::http::request_method;
    using net::http::status;

    struct test_case
    {
        std::string_view name;
        std::string_view response;
        request_method   method = request_method::GET;
        status           expect_status;
        std::string_view expect_body;
    };

    auto tc =
        GENERATE(test_case{.name          = "Content-Length",
                           .response      = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokextra",
                           .expect_status = status::OK,
                           .expect_body   = "ok"},
                 test_case{.name          = "chunked",
                           .response      = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n",
                           .expect_status = status::OK,
                           .expect_body   = "ok"},
                 test_case{.name          = "until closed",
                           .response      = "HTTP/1.1 200 OK\r\n\r\nuntil the end",
                           .expect_status = status::OK,
                           .expect_body   = "until the end"},
                 test_case{.name          = "until closed, after other codings",
                           .response      = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nraw",
                           .expect_status = status::OK,
                           .expect_body   = "raw"},
                 test_case{.name          = "no reason",
                           .response      = "HTTP/1.1 200\r\nContent-Length: 2\r\n\r\nok",
                           .expect_status = status::OK,
                           .expect_body   = "ok"},
                 test_case{.name          = "204",
                           .response      = "HTTP/1.1 204 No Content\r\n\r\nnot the body",
                           .expect_status = status::NO_CONTENT,
                           .expect_body   = ""},
                 test_case{.name          = "304",
                           .response      = "HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\n\r\n",
                           .expect_status = status::NOT_MODIFIED,
                           .expect_body   = ""},
                 test_case{.name          = "HEAD",
                           .response      = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
                           .method        = request_method::HEAD,
                           .expect_status = status::OK,
                           .expect_body   = ""},
                 test_case{.name     = "100 Continue first",
                           .response = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
                           .expect_status = status::OK,
                           .expect_body   = "ok"});

    CAPTURE(tc.name);

    auto resp = decode_response(tc.response, tc.method);
    REQUIRE(resp.has_value());
    CHECK(resp->status_code == tc.expect_status);
    CHECK(read_body(*resp->body) == tc.expect_body);
}

TEST_CASE("malformed responses are rejected", "[http][1.1][response_decode]")
{
    auto response = GENERATE("HTTP/1.1 2000 OK\r\n\r\n"sv,
                             "HTTP/1.1 99 OK\r\n\r\n"sv,
                             "HTTP/1.1 abc OK\r\n\r\n"sv,
                             "HTTP/1.1  200 OK\r\n\r\n"sv,
                             "HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\n"sv,
                             "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"sv,
                             "HTTP/1.1 200 OK\r\nX Y: z\r\n\r\n"sv);
    CAPTURE(response);

    auto resp = decode_response(response);
    REQUIRE_FALSE(resp.has_value());
    CHECK(resp.error() == std::errc::invalid_argument);
}

namespace
{

// encode_request encodes req, returning what was written, or the error.
std::string encode_request(const net::http::client_request& req)
{
    net::io::string_writer<char> out;

    auto res = test::sync_wait(net::http::http11::request_encode(&out, req));
    if (!res.has_value()) return "error: " + res.error().message();
    return out.build();
}

net::http::client_request make_request(net::http::request_method method, std::uint32_t minor = 1)
{
    net::http::client_request req{
        .method  = method,
        .version = {.major = 1, .minor = minor}
    };
    req.uri.host = "example.com";
    req.uri.path = "/";
    return req;
}

std::unique_ptr<net::io::reader> body_of(std::string content)
{
    return std::make_unique<net::io::owned_string_reader>(std::move(content));
}

std::string invalid_argument() { return "error: " + std::make_error_code(std::errc::invalid_argument).message(); }
std::string not_supported() { return "error: " + std::make_error_code(std::errc::not_supported).message(); }
}

TEST_CASE("request bodies are encoded", "[http][1.1][request_encode][body]")
{
    using net::http::request_method;

    SECTION("with a Content-Length")
    {
        auto req = make_request(request_method::POST);
        req.set_body("hello");
        CHECK(encode_request(req) == "POST / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 5\r\n\r\nhello");
    }

    SECTION("only as much as the Content-Length")
    {
        auto req = make_request(request_method::POST);
        req.headers.set_content_length(3);
        req.body = body_of("hello");
        CHECK(encode_request(req) == "POST / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 3\r\n\r\nhel");
    }

    SECTION("chunked, without a Content-Length")
    {
        auto req = make_request(request_method::PUT);
        req.body = body_of("hello");
        CHECK(encode_request(req)
              == "PUT / HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    }

    SECTION("chunked, as asked for")
    {
        auto req = make_request(request_method::POST);
        req.headers.set("Transfer-Encoding"sv, "chunked"sv);
        req.body = body_of("");
        CHECK(encode_request(req)
              == "POST / HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    }

    SECTION("none, but saying so for methods that expect one")
    {
        CHECK(encode_request(make_request(request_method::POST))
              == "POST / HTTP/1.1\r\nHost: example.com\r\nContent-Length: 0\r\n\r\n");
        CHECK(encode_request(make_request(request_method::GET)) == "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");
    }
}

TEST_CASE("request bodies that can't be framed aren't sent", "[http][1.1][request_encode][body]")
{
    using net::http::request_method;

    SECTION("shorter than the Content-Length")
    {
        auto req = make_request(request_method::POST);
        req.headers.set_content_length(10);
        req.body = body_of("hello");
        CHECK(encode_request(req) == invalid_argument());
    }

    SECTION("a Content-Length without a body")
    {
        auto req = make_request(request_method::POST);
        req.headers.set_content_length(5);
        CHECK(encode_request(req) == invalid_argument());
    }

    SECTION("an invalid Content-Length")
    {
        auto req = make_request(request_method::POST);
        req.headers.set("Content-Length"sv, "5x"sv);
        req.body = body_of("hello");
        CHECK(encode_request(req) == invalid_argument());
    }

    SECTION("both Content-Length and Transfer-Encoding")
    {
        auto req = make_request(request_method::POST);
        req.set_body("hello");
        req.headers.set("Transfer-Encoding"sv, "chunked"sv);
        CHECK(encode_request(req) == invalid_argument());
    }

    SECTION("a transfer coding that isn't implemented")
    {
        auto req = make_request(request_method::POST);
        req.headers.set("Transfer-Encoding"sv, "gzip, chunked"sv);
        req.body = body_of("hello");
        CHECK(encode_request(req) == not_supported());
    }

    SECTION("HTTP/1.0 can't send a body of unknown length")
    {
        auto req = make_request(request_method::POST, 0);
        req.body = body_of("hello");
        CHECK(encode_request(req) == invalid_argument());
    }
}

TEST_CASE("encoded request bodies decode", "[http][1.1][request_encode][body]")
{
    std::string content;
    for (int i = 0; content.size() < 100'000; ++i) content += std::to_string(i) + ",";

    auto with_length = GENERATE(true, false);
    CAPTURE(with_length);

    auto req = make_request(net::http::request_method::POST);
    if (with_length) req.set_body(content);
    else req.body = body_of(content);

    // NOTE: the body reads from the reader, so it's kept here (unlike in decode()).
    auto                   encoded = encode_request(req);
    owning_buffered_reader reader{encoded};
    auto                   decoded = test::sync_wait(net::http::http11::request_decode(&reader, 8'192));
    REQUIRE(decoded.has_value());
    CHECK(decoded->headers.is_chunked() == !with_length);
    CHECK(read_body(*decoded->body) == content);
}

TEST_CASE("requests decoded into one request don't keep anything of the last", "[http][1.1][request_decode][reuse]")
{
    // the first has a chunked body, a query, and a field the second doesn't; the second a length; the third nothing.
    net::io::string_reader<char> content("POST /a?x=1 HTTP/1.1\r\n"
                                         "Host: first\r\n"
                                         "X-Only-First: yes\r\n"
                                         "Transfer-Encoding: chunked\r\n"
                                         "\r\n"
                                         "3\r\nabc\r\n0\r\n\r\n"
                                         "PUT http://second:8080/b HTTP/1.1\r\n"
                                         "Host: ignored\r\n"
                                         "Content-Length: 2\r\n"
                                         "\r\n"
                                         "de"
                                         "GET /c HTTP/1.0\r\n"
                                         "\r\n");

    // NOTE: a small buffer, so that some heads are decoded as they're read, and others from the buffer.
    auto buffer_size = GENERATE(std::size_t{16}, std::size_t{4'096});
    CAPTURE(buffer_size);

    net::io::buffered_reader  reader(&content, buffer_size);
    net::http::server_request req;

    // body reads the request's body to its end.
    const auto body = [&]
    {
        std::string out;
        while (true)
        {
            std::array<char, 8> chunk{};
            auto                res = test::sync_wait(req.body->read(std::as_writable_bytes(std::span{chunk})));
            out.append(chunk.data(), res.count);
            if (res.err)
            {
                CHECK(res.err == net::io::errc::end_of_stream);
                return out;
            }
        }
    };

    REQUIRE_FALSE(test::sync_wait(net::http::http11::request_decode(&reader, 8'192, req)));
    CHECK(req.method == net::http::request_method::POST);
    CHECK(req.target.path == "/a");
    CHECK(req.target.query == "x=1");
    CHECK(req.host == "first");
    CHECK(req.headers.get("X-Only-First"sv) == "yes");
    CHECK(body() == "abc");

    REQUIRE_FALSE(test::sync_wait(net::http::http11::request_decode(&reader, 8'192, req)));
    CHECK(req.method == net::http::request_method::PUT);
    CHECK(req.target.path == "/b");
    CHECK(req.target.query.empty());
    CHECK(req.target.port == "8080");
    CHECK(req.host == "second");
    CHECK(req.headers
          == net::http::headers{
              {          "Host", {"ignored"}},
              {"Content-Length",       {"2"}}
    });
    CHECK(body() == "de");

    REQUIRE_FALSE(test::sync_wait(net::http::http11::request_decode(&reader, 8'192, req)));
    CHECK(req.method == net::http::request_method::GET);
    CHECK(req.version == net::http::protocol_version{.major = 1, .minor = 0});
    CHECK(req.target == net::http::request_target{.path = "/c"});
    CHECK(req.host.empty());
    CHECK(req.headers.empty());
    CHECK(body().empty());
}

TEST_CASE("a response head that doesn't fit in the writer's buffer is still written", "[http][1.1][response_encode]")
{
    net::http::server_response resp{
        .version     = {.major = 1, .minor = 1},
        .status_code = net::http::status::OK,
    };
    resp.headers.set("X-Long"sv, std::string(200, 'x'));

    const auto want = "HTTP/1.1 200 OK\r\nX-Long: " + std::string(200, 'x') + "\r\n\r\n";

    // with room for it, and without.
    auto buffer_size = GENERATE(std::size_t{64}, std::size_t{4'096});
    CAPTURE(buffer_size);

    net::io::string_writer<char> out;
    net::io::buffered_writer     writer{&out, buffer_size};

    auto res = test::sync_wait(net::http::http11::response_encode(&writer, resp));
    REQUIRE(res.has_value());
    REQUIRE_FALSE(test::sync_wait(writer.flush()).err);
    CHECK(out.build() == want);
}

TEST_CASE("a field line longer than the reader's buffer is still decoded", "[http][1.1][request_decode]")
{
    const std::string value(200, 'v');
    const std::string input = "GET / HTTP/1.1\r\nHost: a\r\nX-Long: " + value + "\r\n\r\n";

    net::io::string_reader<char> content(input);
    net::io::buffered_reader     reader(&content, 32);
    net::http::server_request    req;

    REQUIRE_FALSE(test::sync_wait(net::http::http11::request_decode(&reader, 8'192, req)));
    CHECK(req.headers.get("X-Long"sv) == value);
}

TEST_CASE("a malformed request line fails without waiting for the rest of the head", "[http][1.1][request_decode]")
{
    // the head never ends: had decoding waited for it, it would fail with closed, at the end of the input.
    net::io::string_reader<char> content("NOT A REQUEST\r\nHost: a\r\n");
    net::io::buffered_reader     reader(&content, 8);
    net::http::server_request    req;

    auto err = test::sync_wait(net::http::http11::request_decode(&reader, 8'192, req));
    CHECK(err == std::errc::invalid_argument);
}

TEST_CASE("a request gets one final response, after any interim ones", "[http][1.1][response_writer]")
{
    net::io::string_writer<char> out;
    net::http::server_response   resp{
        .version = {.major = 1, .minor = 1},
        .body    = &out
    };
    net::http::response_writer rw{&out, &resp, net::http::http11::response_encode};

    CHECK_FALSE(rw.sent());

    // interim responses don't count.
    (void)test::sync_wait(rw.send(net::http::status::CONTINUE, 0));
    CHECK_FALSE(rw.sent());

    (void)test::sync_wait(rw.send(net::http::status::OK, 0));
    CHECK(rw.sent());

    CHECK_THROWS_AS(test::sync_wait(rw.send(net::http::status::NOT_FOUND, 0)), std::logic_error);
    CHECK(out.build().contains("HTTP/1.1 200 OK"));
    CHECK_FALSE(out.build().contains("404"));
}
