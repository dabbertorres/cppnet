#include "http/http11.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/sync_wait.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"
#include "io/string_reader.hpp"
#include "io/string_writer.hpp"
#include "string_makers.hpp"
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
        CHECK(request.headers == net::http::headers{{"Host", {"a"}}});
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
    CHECK(read_result.err == net::io::status_condition::closed);
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
    CHECK(read_result.err == net::io::status_condition::closed);

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
        .uri     = parsed.to_value(),
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
        std::string_view   name;
        std::string_view   request;
        std::errc          expect = std::errc::invalid_argument;
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
        test_case{.name = "unknown method", .request = "BREW / HTTP/1.1\r\nHost: a\r\n\r\n", .expect = std::errc::not_supported},
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
        test_case{.name = "Content-Length with junk", .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5x\r\n\r\n"},
        test_case{.name = "negative Content-Length", .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n"},
        test_case{.name = "signed Content-Length", .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: +5\r\n\r\n"},
        test_case{.name = "empty Content-Length", .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length:\r\n\r\n"},
        test_case{.name = "huge Content-Length", .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999999\r\n\r\n"},
        test_case{.name = "different Content-Lengths", .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n"},
        test_case{.name = "different Content-Lengths in one line", .request = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 6\r\n\r\n"},
        test_case{.name = "Transfer-Encoding and Content-Length", .request = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n"},
        test_case{.name = "chunked isn't last", .request = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked, gzip\r\n\r\n"},
        test_case{.name = "no chunked", .request = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n"},
        test_case{.name = "not quite chunked", .request = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: xchunked\r\n\r\n"},
        test_case{.name = "chunked twice", .request = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n"},
        test_case{.name = "Transfer-Encoding in HTTP/1.0", .request = "POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n"},
        test_case{.name = "unsupported coding", .request = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", .expect = std::errc::not_supported});

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
    CHECK(decode("GET / HTTP/1.1\r\nHost: a\r\nX: " + long_value + "\r\n\r\n", 64).error() == std::errc::value_too_large);

    // many small lines add up.
    std::string many = "GET / HTTP/1.1\r\nHost: a\r\n";
    for (int i = 0; i < 20; ++i) many += "X: y\r\n";
    CHECK(decode(many + "\r\n", 64).error() == std::errc::value_too_large);

    // a line without an end doesn't buffer forever.
    CHECK(decode("GET / HTTP/1.1\r\nHost: a\r\nX: " + std::string(10'000, 'v'), 64).error() == std::errc::value_too_large);

    CHECK(decode("GET / HTTP/1.1\r\nHost: a\r\n\r\n", 64).has_value());
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
    net::http::server_response   resp{.version = {.major = 1, .minor = 1}, .status_code = net::http::status::OK};
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
    return test::sync_wait(
        net::http::http11::response_decode(std::make_unique<owning_buffered_reader>(text), method));
}

std::string read_body(net::io::reader& body)
{
    std::string out;
    std::string buf(64, '\0');

    while (true)
    {
        auto res = test::sync_wait(body.read(buf));
        out.append(buf.data(), res.count);
        if (res.err) return res.err == net::io::status_condition::closed ? out : out + " [" + res.err.message() + "]";
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

    auto tc = GENERATE(
        test_case{.name = "Content-Length", .response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokextra", .expect_status = status::OK, .expect_body = "ok"},
        test_case{.name = "chunked", .response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n", .expect_status = status::OK, .expect_body = "ok"},
        test_case{.name = "until closed", .response = "HTTP/1.1 200 OK\r\n\r\nuntil the end", .expect_status = status::OK, .expect_body = "until the end"},
        test_case{.name = "until closed, after other codings", .response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nraw", .expect_status = status::OK, .expect_body = "raw"},
        test_case{.name = "no reason", .response = "HTTP/1.1 200\r\nContent-Length: 2\r\n\r\nok", .expect_status = status::OK, .expect_body = "ok"},
        test_case{.name = "204", .response = "HTTP/1.1 204 No Content\r\n\r\nnot the body", .expect_status = status::NO_CONTENT, .expect_body = ""},
        test_case{.name = "304", .response = "HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\n\r\n", .expect_status = status::NOT_MODIFIED, .expect_body = ""},
        test_case{.name = "HEAD", .response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", .method = request_method::HEAD, .expect_status = status::OK, .expect_body = ""},
        test_case{.name = "100 Continue first", .response = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", .expect_status = status::OK, .expect_body = "ok"});

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
