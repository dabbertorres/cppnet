#include "websocket/handshake.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "encoding/base64.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "url.hpp"
#include "websocket/error.hpp"
#include "websocket/websocket.hpp"

namespace
{

using namespace std::string_view_literals;

using net::websocket::errc;

namespace ws   = net::websocket::detail;
namespace http = net::http;

// RFC 6455 4.2.1's key, and 4.2.2's accept for it.
constexpr auto example_key    = "dGhlIHNhbXBsZSBub25jZQ=="sv;
constexpr auto example_accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="sv;

using field_list = std::vector<std::pair<std::string_view, std::string_view>>;

// change changes fields: each replaces the field it names (an empty value removes it), and a name given twice is two
// lines.
void change(http::headers& headers, const field_list& changes)
{
    std::vector<std::string_view> changed;
    for (const auto& [name, value] : changes)
    {
        if (std::ranges::contains(changed, name))
        {
            headers.add(name, value);
            continue;
        }

        changed.push_back(name);
        if (value.empty()) headers.remove(name);
        else headers.set(name, value);
    }
}

// handshake is a valid WebSocket handshake request, as the HTTP/1.1 decoder leaves it, with fields changed (see
// change()).
http::server_request handshake(const field_list& changes = {})
{
    http::server_request req;
    req.method  = http::request_method::GET;
    req.version = {.major = 1, .minor = 1};
    req.target  = {.path = "/chat"};
    req.host    = "server.example.com";

    req.headers.set("Host"sv, "server.example.com"sv);
    req.headers.set("Upgrade"sv, "websocket"sv);
    req.headers.set("Connection"sv, "Upgrade"sv);
    req.headers.set("Sec-WebSocket-Key"sv, example_key);
    req.headers.set("Sec-WebSocket-Version"sv, "13"sv);

    change(req.headers, changes);
    return req;
}

// response is a valid response to the example key's handshake, with fields changed (see change()).
http::client_response response(const field_list& changes = {}, http::status status = http::status::SWITCHING_PROTOCOLS)
{
    http::client_response out;
    out.version     = {.major = 1, .minor = 1};
    out.status_code = status;
    out.headers.set("Upgrade"sv, "websocket"sv);
    out.headers.set("Connection"sv, "Upgrade"sv);
    out.headers.set("Sec-WebSocket-Accept"sv, example_accept);

    change(out.headers, changes);
    return out;
}

}

TEST_CASE("accept_key is RFC 6455's", "[websocket][handshake]")
{
    CHECK(ws::accept_key(example_key) == example_accept);
}

TEST_CASE("check_request takes a WebSocket handshake", "[websocket][handshake]")
{
    struct test_case
    {
        std::string_view name;
        field_list       changes{};
    };

    auto tc = GENERATE(
        test_case{
            .name = "as RFC 6455's example"
    },
        test_case{.name = "tokens in any case", .changes = {{"Upgrade", "WebSocket"}, {"Connection", "upgrade"}}},
        test_case{.name    = "tokens in lists",
                  .changes = {{"Upgrade", "foo/2, websocket"}, {"Connection", "keep-alive, Upgrade"}}},
        test_case{.name    = "tokens over several lines",
                  .changes = {{"Connection", "keep-alive"}, {"Connection", "Upgrade"}}},
        test_case{.name = "a Content-Length of 0", .changes = {{"Content-Length", "0"}}},
        test_case{.name    = "other fields",
                  .changes = {{"Origin", "http://example.com"},
                              {"Sec-WebSocket-Protocol", "chat"},
                              {"Sec-WebSocket-Extensions", "permessage-deflate"}}});

    CAPTURE(tc.name);

    // NOTE: the key is a view of the request's fields.
    const auto req = handshake(tc.changes);
    const auto key = ws::check_request(req);
    REQUIRE(key.has_value());
    CHECK(*key == example_key);
}

TEST_CASE("check_request refuses what isn't a WebSocket handshake", "[websocket][handshake]")
{
    struct test_case
    {
        std::string_view       name;
        field_list             changes{};
        errc                   expect;
        http::request_method   method = http::request_method::GET;
        http::protocol_version version{.major = 1, .minor = 1};
    };

    auto tc = GENERATE(
        test_case{
            .name   = "POST",
            .expect = errc::not_websocket,
            .method = http::request_method::POST
    },
        test_case{.name = "CONNECT", .expect = errc::not_websocket, .method = http::request_method::CONNECT},
        test_case{.name = "HTTP/1.0", .expect = errc::not_websocket, .version = {.major = 1, .minor = 0}},
        test_case{.name    = "HTTP/2, not an extended CONNECT",
                  .expect  = errc::not_websocket,
                  .version = {.major = 2, .minor = 0}},
        test_case{.name = "no Upgrade", .changes = {{"Upgrade", ""}}, .expect = errc::not_websocket},
        test_case{.name = "another Upgrade", .changes = {{"Upgrade", "h2c"}}, .expect = errc::not_websocket},
        test_case{.name    = "Upgrade, not a token of it",
                  .changes = {{"Upgrade", "websockets"}},
                  .expect  = errc::not_websocket},
        test_case{.name = "no Connection", .changes = {{"Connection", ""}}, .expect = errc::not_websocket},
        test_case{.name    = "Connection without upgrade",
                  .changes = {{"Connection", "keep-alive"}},
                  .expect  = errc::not_websocket},
        test_case{.name = "a Content-Length", .changes = {{"Content-Length", "5"}}, .expect = errc::not_websocket},
        test_case{.name    = "a Transfer-Encoding",
                  .changes = {{"Transfer-Encoding", "chunked"}},
                  .expect  = errc::not_websocket},
        test_case{.name = "no version", .changes = {{"Sec-WebSocket-Version", ""}}, .expect = errc::bad_version},
        test_case{.name = "version 8", .changes = {{"Sec-WebSocket-Version", "8"}}, .expect = errc::bad_version},
        test_case{.name    = "version 13 in a list",
                  .changes = {{"Sec-WebSocket-Version", "13, 8"}},
                  .expect  = errc::bad_version},
        test_case{.name    = "version on two lines",
                  .changes = {{"Sec-WebSocket-Version", "13"}, {"Sec-WebSocket-Version", "13"}},
                  .expect  = errc::bad_version},
        test_case{.name = "no key", .changes = {{"Sec-WebSocket-Key", ""}}, .expect = errc::bad_key},
        test_case{.name    = "a key that isn't base64",
                  .changes = {{"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ!="}},
                  .expect  = errc::bad_key},
        test_case{.name    = "a key whose padding bits aren't 0",
                  .changes = {{"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZR=="}},
                  .expect  = errc::bad_key},
        test_case{.name    = "a key without padding",
                  .changes = {{"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ"}},
                  .expect  = errc::bad_key},
        test_case{.name    = "a key of 15 bytes",
                  .changes = {{"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25j"}},
                  .expect  = errc::bad_key},
        test_case{.name    = "a key of 17 bytes",
                  .changes = {{"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZXM="}},
                  .expect  = errc::bad_key},
        test_case{.name    = "a key of 18 bytes",
                  .changes = {{"Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZXMh"}},
                  .expect  = errc::bad_key},
        test_case{.name    = "keys on two lines",
                  .changes = {{"Sec-WebSocket-Key", example_key}, {"Sec-WebSocket-Key", example_key}},
                  .expect  = errc::bad_key});

    CAPTURE(tc.name);

    auto req    = handshake(tc.changes);
    req.method  = tc.method;
    req.version = tc.version;

    const auto key = ws::check_request(req);
    REQUIRE_FALSE(key.has_value());
    CHECK(key.error() == tc.expect);
}

TEST_CASE("check_request takes an HTTP/2 extended CONNECT, as the server presents it", "[websocket][handshake][http2]")
{
    struct test_case
    {
        std::string_view name;
        std::string_view protocol = "websocket";
        field_list       fields{
            {"Sec-WebSocket-Version", "13"}
        };
        std::optional<errc>  expect{};
        http::request_method method = http::request_method::GET;
    };

    auto tc = GENERATE(
        test_case{
            .name = "taken, without a key, Upgrade or Connection"
    },
        test_case{.name = "the protocol in any case", .protocol = "WebSocket"},
        test_case{.name   = "a Content-Length of 0",
                  .fields = {{"Sec-WebSocket-Version", "13"}, {"Content-Length", "0"}}},
        test_case{.name = "another protocol", .protocol = "webtransport", .expect = errc::not_websocket},
        test_case{.name = "no protocol", .protocol = "", .expect = errc::not_websocket},
        test_case{.name = "not GET", .expect = errc::not_websocket, .method = http::request_method::CONNECT},
        test_case{.name   = "a body",
                  .fields = {{"Sec-WebSocket-Version", "13"}, {"Content-Length", "5"}},
                  .expect = errc::not_websocket},
        test_case{.name = "no version", .fields = {}, .expect = errc::bad_version},
        test_case{.name = "version 8", .fields = {{"Sec-WebSocket-Version", "8"}}, .expect = errc::bad_version});

    CAPTURE(tc.name);

    http::server_request req;
    req.method   = tc.method;
    req.version  = {.major = 2, .minor = 0};
    req.protocol = tc.protocol;
    for (const auto& [name, value] : tc.fields) req.headers.add(name, value);

    const auto key = ws::check_request(req);
    if (tc.expect)
    {
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error() == *tc.expect);
    }
    else
    {
        REQUIRE(key.has_value());
        CHECK(key->empty());
    }
}

TEST_CASE("select_subprotocol takes the server's first that the client offers", "[websocket][handshake]")
{
    const std::vector<std::string> server{"v2.chat", "chat"};

    struct test_case
    {
        std::string_view name;
        field_list       offered{};
        std::string_view expect;
    };

    auto tc = GENERATE(
        test_case{
            .name   = "none offered",
            .expect = ""
    },
        test_case{.name = "one", .offered = {{"Sec-WebSocket-Protocol", "chat"}}, .expect = "chat"},
        test_case{.name    = "the server's order",
                  .offered = {{"Sec-WebSocket-Protocol", "chat, v2.chat"}},
                  .expect  = "v2.chat"},
        test_case{.name    = "over several lines",
                  .offered = {{"Sec-WebSocket-Protocol", "other, chat"}, {"Sec-WebSocket-Protocol", "v2.chat"}},
                  .expect  = "v2.chat"},
        test_case{.name = "none of the server's", .offered = {{"Sec-WebSocket-Protocol", "mqtt, wamp"}}, .expect = ""},
        test_case{.name = "exactly, not ignoring case", .offered = {{"Sec-WebSocket-Protocol", "Chat"}}, .expect = ""});

    CAPTURE(tc.name);

    const auto req    = handshake(tc.offered);
    const auto chosen = ws::select_subprotocol(server, req.headers);
    CHECK(chosen == tc.expect);
    // a view of the server's own.
    if (!chosen.empty())
        CHECK(std::ranges::any_of(server, [&](const std::string& s) { return s.data() == chosen.data(); }));

    CHECK(ws::select_subprotocol({}, req.headers).empty());
}

TEST_CASE("check_origin allows the request's own origin, and patterns", "[websocket][handshake][origin]")
{
    struct test_case
    {
        std::string_view         name;
        field_list               changes{};
        std::vector<std::string> origins{};
        bool                     allowed;
        bool                     any    = false;
        std::string_view         target = {}; // an absolute-form target's host and port
        std::string_view         port   = {};
    };

    auto tc = GENERATE(
        test_case{
            .name    = "no Origin",
            .allowed = true
    },
        test_case{.name = "the same host", .changes = {{"Origin", "https://server.example.com"}}, .allowed = true},
        test_case{.name    = "the same host, ignoring case",
                  .changes = {{"Origin", "http://Server.Example.COM"}},
                  .allowed = true},
        test_case{.name    = "the same host and port",
                  .changes = {{"Host", "server.example.com:8080"}, {"Origin", "http://server.example.com:8080"}},
                  .allowed = true},
        test_case{.name    = "the default port, left out of the origin",
                  .changes = {{"Host", "server.example.com:443"}, {"Origin", "https://server.example.com"}},
                  .allowed = true},
        test_case{.name    = "the default port, left out of Host",
                  .changes = {{"Origin", "http://server.example.com:80"}},
                  .allowed = true},
        test_case{.name = "another port", .changes = {{"Origin", "http://server.example.com:8080"}}, .allowed = false},
        test_case{.name    = "another port, in Host",
                  .changes = {{"Host", "server.example.com:8080"}, {"Origin", "http://server.example.com"}},
                  .allowed = false},
        test_case{.name = "another host", .changes = {{"Origin", "https://evil.example.com"}}, .allowed = false},
        test_case{.name = "a suffix of the host", .changes = {{"Origin", "https://example.com"}}, .allowed = false},
        test_case{.name    = "the host as a suffix",
                  .changes = {{"Origin", "https://server.example.com.evil.net"}},
                  .allowed = false},
        test_case{.name    = "an absolute target's host",
                  .changes = {{"Origin", "http://other.example.com:81"}},
                  .allowed = true,
                  .target  = "other.example.com",
                  .port    = "81"},
        test_case{.name    = "IPv6",
                  .changes = {{"Host", "[::1]:8080"}, {"Origin", "http://[::1]:8080"}},
                  .allowed = true},
        test_case{.name = "null", .changes = {{"Origin", "null"}}, .allowed = false},
        test_case{.name = "without a scheme", .changes = {{"Origin", "server.example.com"}}, .allowed = false},
        test_case{.name = "with a path", .changes = {{"Origin", "https://server.example.com/"}}, .allowed = false},
        test_case{.name = "without a host", .changes = {{"Origin", "https://"}}, .allowed = false},
        test_case{.name = "a bad port", .changes = {{"Origin", "https://server.example.com:x"}}, .allowed = false},
        test_case{.name    = "two lines",
                  .changes = {{"Origin", "https://server.example.com"}, {"Origin", "https://server.example.com"}},
                  .allowed = false},
        test_case{.name    = "a pattern",
                  .changes = {{"Origin", "https://app.example.com"}},
                  .origins = {"other.net", "*.example.com"},
                  .allowed = true},
        test_case{.name    = "a pattern, ignoring case",
                  .changes = {{"Origin", "https://APP.example.com"}},
                  .origins = {"app.Example.com"},
                  .allowed = true},
        test_case{.name    = "a pattern's * is a label, not none",
                  .changes = {{"Origin", "https://example.com"}},
                  .origins = {"*.example.com"},
                  .allowed = false},
        test_case{.name    = "a pattern's * is a label, not several",
                  .changes = {{"Origin", "https://a.b.example.com"}},
                  .origins = {"*.example.com"},
                  .allowed = false},
        test_case{.name    = "a pattern with a port",
                  .changes = {{"Origin", "http://localhost:3000"}},
                  .origins = {"localhost:3000"},
                  .allowed = true},
        test_case{.name    = "a pattern without a port is for the default",
                  .changes = {{"Origin", "http://localhost:3000"}},
                  .origins = {"localhost"},
                  .allowed = false},
        test_case{.name    = "a pattern without a port, the default",
                  .changes = {{"Origin", "http://localhost"}},
                  .origins = {"localhost"},
                  .allowed = true},
        test_case{.name    = "a pattern with any port",
                  .changes = {{"Origin", "http://localhost:3000"}},
                  .origins = {"localhost:*"},
                  .allowed = true},
        test_case{.name    = "a pattern with another port",
                  .changes = {{"Origin", "http://localhost:3001"}},
                  .origins = {"localhost:3000"},
                  .allowed = false},
        test_case{.name    = "a pattern that matches nothing",
                  .changes = {{"Origin", "https://evil.example.com"}},
                  .origins = {"app.example.com"},
                  .allowed = false},
        test_case{.name    = "insecure_any_origin",
                  .changes = {{"Origin", "https://evil.net"}},
                  .allowed = true,
                  .any     = true},
        test_case{.name = "insecure_any_origin, null", .changes = {{"Origin", "null"}}, .allowed = true, .any = true});

    CAPTURE(tc.name);

    auto req = handshake(tc.changes);
    if (!tc.target.empty())
    {
        req.target.kind = http::request_target::form::absolute;
        req.target.host = tc.target;
        req.target.port = tc.port;
    }

    const auto err = ws::check_origin(req, {.origins = tc.origins, .insecure_any_origin = tc.any});
    if (tc.allowed) CHECK_FALSE(err);
    else CHECK(err == errc::origin_refused);
}

TEST_CASE("make_request makes a client's handshake", "[websocket][handshake][client]")
{
    const auto target = net::url::parse("http://server.example.com:8080/chat?room=1"sv);
    REQUIRE(target.has_value());

    SECTION("the handshake's fields, and a fresh key")
    {
        const auto first = ws::make_request(*target, {});
        CHECK(first.request.method == http::request_method::GET);
        CHECK(first.request.version == http::protocol_version{.major = 1, .minor = 1});
        CHECK(first.request.uri.build() == target->build());
        CHECK(first.request.headers.get("Upgrade"sv) == "websocket"sv);
        CHECK(first.request.headers.get("Connection"sv) == "Upgrade"sv);
        CHECK(first.request.headers.get("Sec-WebSocket-Version"sv) == "13"sv);
        CHECK(first.request.headers.get("Sec-WebSocket-Key"sv) == first.key);
        CHECK_FALSE(first.request.headers.get("Sec-WebSocket-Protocol"sv).has_value());
        CHECK_FALSE(first.request.headers.get("Origin"sv).has_value());
        CHECK_FALSE(first.request.body);

        const auto decoded = net::encoding::base64::encoding{}.decode(first.key);
        REQUIRE(decoded.has_value());
        CHECK(decoded->size() == 16);

        // so a server takes it.
        http::server_request req;
        req.method  = first.request.method;
        req.version = first.request.version;
        for (const auto& [name, values] : first.request.headers)
            for (const auto& value : values) req.headers.add(name, value);
        const auto key = ws::check_request(req);
        REQUIRE(key.has_value());
        CHECK(*key == first.key);

        const auto second = ws::make_request(*target, {});
        CHECK(second.key != first.key);
    }

    SECTION("the options' fields")
    {
        net::websocket::connect_options options{
            .subprotocols = {"v2.chat", "chat"},
            .origin       = "https://app.example.com"
        };
        options.headers.set("Authorization"sv, "Bearer x"sv);
        options.headers.set("Upgrade"sv, "h2c"sv);
        options.headers.set("Sec-WebSocket-Key"sv, example_key);
        options.headers.set("Sec-WebSocket-Extensions"sv, "permessage-deflate"sv);

        const auto out = ws::make_request(*target, options);
        CHECK(out.request.headers.get("Authorization"sv) == "Bearer x"sv);
        CHECK(out.request.headers.get("Upgrade"sv) == "websocket"sv);
        CHECK(out.request.headers.get("Sec-WebSocket-Key"sv) == out.key);
        CHECK(out.key != example_key);
        CHECK_FALSE(out.request.headers.get("Sec-WebSocket-Extensions"sv).has_value());
        CHECK(out.request.headers.get("Sec-WebSocket-Protocol"sv) == "v2.chat, chat"sv);
        CHECK(out.request.headers.get("Origin"sv) == "https://app.example.com"sv);
    }
}

TEST_CASE("check_response takes a server's switch", "[websocket][handshake][client]")
{
    const std::vector<std::string> offered{"v2.chat", "chat"};

    SECTION("without a subprotocol")
    {
        const auto res    = response();
        const auto chosen = ws::check_response(res, example_key, offered);
        REQUIRE(chosen.has_value());
        CHECK(chosen->empty());
    }

    SECTION("with one offered, and tokens in any case")
    {
        // NOTE: the choice is a view of the response's fields.
        const auto res    = response({
            {"Sec-WebSocket-Protocol",      "chat"},
            {               "Upgrade", "WebSocket"},
            {            "Connection",   "upgrade"}
        });
        const auto chosen = ws::check_response(res, example_key, offered);
        REQUIRE(chosen.has_value());
        CHECK(*chosen == "chat");
    }
}

TEST_CASE("check_response refuses what the client must fail", "[websocket][handshake][client]")
{
    struct test_case
    {
        std::string_view         name;
        field_list               changes{};
        errc                     expect;
        http::status             status = http::status::SWITCHING_PROTOCOLS;
        std::vector<std::string> offered{"chat"};
    };

    auto tc = GENERATE(
        test_case{
            .name   = "200",
            .expect = errc::bad_status,
            .status = http::status::OK
    },
        test_case{.name    = "302",
                  .changes = {{"Location", "/elsewhere"}},
                  .expect  = errc::bad_status,
                  .status  = http::status::FOUND},
        test_case{.name = "426", .expect = errc::bad_status, .status = http::status::UPGRADE_REQUIRED},
        test_case{.name = "no Upgrade", .changes = {{"Upgrade", ""}}, .expect = errc::not_websocket},
        test_case{.name = "another Upgrade", .changes = {{"Upgrade", "h2c"}}, .expect = errc::not_websocket},
        test_case{.name = "no Connection", .changes = {{"Connection", ""}}, .expect = errc::not_websocket},
        test_case{.name = "no accept", .changes = {{"Sec-WebSocket-Accept", ""}}, .expect = errc::bad_accept},
        test_case{.name    = "another accept",
                  .changes = {{"Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOQ="}},
                  .expect  = errc::bad_accept},
        test_case{.name    = "the accept in another case",
                  .changes = {{"Sec-WebSocket-Accept", "S3PPLMBITXAQ9KYGZZHZRBK+XOO="}},
                  .expect  = errc::bad_accept},
        test_case{.name    = "the accept on two lines",
                  .changes = {{"Sec-WebSocket-Accept", example_accept}, {"Sec-WebSocket-Accept", example_accept}},
                  .expect  = errc::bad_accept},
        test_case{.name    = "an extension",
                  .changes = {{"Sec-WebSocket-Extensions", "permessage-deflate"}},
                  .expect  = errc::bad_extension},
        test_case{.name    = "a subprotocol not offered",
                  .changes = {{"Sec-WebSocket-Protocol", "mqtt"}},
                  .expect  = errc::bad_subprotocol},
        test_case{.name    = "a subprotocol in another case",
                  .changes = {{"Sec-WebSocket-Protocol", "Chat"}},
                  .expect  = errc::bad_subprotocol},
        test_case{.name    = "two subprotocols",
                  .changes = {{"Sec-WebSocket-Protocol", "chat, chat"}},
                  .expect  = errc::bad_subprotocol},
        test_case{.name    = "subprotocols on two lines",
                  .changes = {{"Sec-WebSocket-Protocol", "chat"}, {"Sec-WebSocket-Protocol", "chat"}},
                  .expect  = errc::bad_subprotocol},
        test_case{.name    = "a subprotocol when none was offered",
                  .changes = {{"Sec-WebSocket-Protocol", "chat"}},
                  .expect  = errc::bad_subprotocol,
                  .offered = {}});

    CAPTURE(tc.name);

    const auto res    = response(tc.changes, tc.status);
    const auto chosen = ws::check_response(res, example_key, tc.offered);
    REQUIRE_FALSE(chosen.has_value());
    CHECK(chosen.error() == tc.expect);
}
