#include "http/request_target.hpp"

#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/string_writer.hpp"
#include "url.hpp"

namespace
{

using namespace std::string_view_literals;

using net::http::request_method;
using net::http::request_target;
using form = net::http::request_target::form;

}

TEST_CASE("valid request targets", "[http][request_target]")
{
    // NOLINTBEGIN(clang-diagnostic-missing-designated-field-initializers): the rest default to empty.
    struct test_case
    {
        request_method   method = request_method::GET;
        std::string_view target;
        request_target   expect;
    };

    auto tc = GENERATE(
        test_case{
            .target = "/",
            .expect = {.path = "/"}
    },
        test_case{.target = "/a/b?x=1&y", .expect = {.path = "/a/b", .query = "x=1&y"}},
        // nothing is decoded, and colons and '@' are just path characters.
        test_case{.target = "/a%2Fb/x:run", .expect = {.path = "/a%2Fb/x:run"}},
        test_case{.target = "/search?q=a:b&email=a@b.com",
                  .expect = {.path = "/search", .query = "q=a:b&email=a@b.com"}},
        test_case{.target = "/?a?b", .expect = {.path = "/", .query = "a?b"}},
        // dot segments, encoded or not, can't go above the root.
        test_case{.target = "/a/./b", .expect = {.path = "/a/b"}},
        test_case{.target = "/a/b/..", .expect = {.path = "/a/"}},
        test_case{.target = "/a/../../..", .expect = {.path = "/"}},
        test_case{.target = "/files/%2e%2E/%2e%2e/etc/passwd", .expect = {.path = "/etc/passwd"}},
        test_case{.target = "/a/.%2e/b", .expect = {.path = "/b"}},
        test_case{.target = "/a/..b/c", .expect = {.path = "/a/..b/c"}},
        test_case{
            .target = "http://Example.com:8080/p?q",
            .expect = {.kind = form::absolute, .path = "/p", .query = "q", .host = "Example.com", .port = "8080"},
        },
        test_case{.target = "https://example.com",
                  .expect = {.kind = form::absolute, .path = "/", .host = "example.com"}},
        test_case{.target = "http://[::1]:80/",
                  .expect = {.kind = form::absolute, .path = "/", .host = "::1", .port = "80"}},
        test_case{
            .method = request_method::CONNECT,
            .target = "example.com:443",
            .expect = {.kind = form::authority, .path = "/", .host = "example.com", .port = "443"},
        },
        test_case{.method = request_method::OPTIONS, .target = "*", .expect = {.kind = form::asterisk, .path = "/"}});
    // NOLINTEND(clang-diagnostic-missing-designated-field-initializers)

    CAPTURE(tc.target);

    auto result = net::http::parse_request_target(tc.method, tc.target);
    REQUIRE(result.has_value());
    CHECK(result->kind == tc.expect.kind);
    CHECK(result->path == tc.expect.path);
    CHECK(result->query == tc.expect.query);
    CHECK(result->host == tc.expect.host);
    CHECK(result->port == tc.expect.port);
}

TEST_CASE("invalid request targets", "[http][request_target]")
{
    struct test_case
    {
        request_method   method = request_method::GET;
        std::string_view target;
    };

    auto tc = GENERATE(test_case{.target = ""},
                       test_case{.target = "a/b"},
                       test_case{.target = "/%"},
                       test_case{.target = "/%zz"},
                       test_case{.target = "/a b"},
                       test_case{.target = "/a\r\nX: y"},
                       test_case{.target = "/a#fragment"},
                       test_case{.target = "/a\"b"},
                       test_case{.target = "/\xc3\xa9"},
                       test_case{.target = "/?q=%"},
                       test_case{.target = "*"},
                       test_case{.target = "ftp://example.com/"},
                       test_case{.target = "http://user@example.com/"},
                       test_case{.target = "http:///path"},
                       test_case{.target = "http://example.com:99999/"},
                       test_case{.target = "http://example.com:http/"},
                       test_case{.target = "http://[not-an-ip]/"},
                       test_case{.method = request_method::CONNECT, .target = "example.com"},
                       test_case{.method = request_method::CONNECT, .target = "/path"},
                       test_case{.method = request_method::OPTIONS, .target = "**"});

    CAPTURE(tc.target);

    auto result = net::http::parse_request_target(tc.method, tc.target);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == std::errc::invalid_argument);
}

TEST_CASE("query values are decoded", "[http][request_target]")
{
    auto result = net::http::parse_request_target(request_method::GET, "/?a=1&a=x+y%26z&b&&c="sv);
    REQUIRE(result.has_value());

    CHECK(result->query_values()
          == net::url::query_values{
              {"a", {"1", "x y&z"}},
              {"b",             {}},
              {"c",           {""}},
    });
}

TEST_CASE("the router matches decoded segments", "[http][router]")
{
    std::string matched;

    net::http::router router;
    router.GET("/x/y",
               [&](const auto&, auto&) -> net::coro::task<>
               {
                   matched = "/x/y";
                   co_return;
               });
    router.GET("/x/a b",
               [&](const auto&, auto&) -> net::coro::task<>
               {
                   matched = "/x/a b";
                   co_return;
               });

    const auto route = [&](std::string_view target)
    {
        matched.clear();

        net::http::server_request req{.method = request_method::GET};
        auto                      parsed = net::http::parse_request_target(req.method, target);
        REQUIRE(parsed.has_value());
        req.target = *parsed;

        net::io::string_writer<char> out;
        net::http::server_response   resp{
            .version = {.major = 1, .minor = 1},
            .body    = &out
        };
        net::http::response_writer rw{&out, &resp, net::http::http11::response_encode};
        test::sync_wait(router(req, rw));

        return matched.empty() ? out.build().substr(0, 12) : matched;
    };

    CHECK(route("/x/y") == "/x/y");
    CHECK(route("/x/%79") == "/x/y");
    CHECK(route("/x/a%20b") == "/x/a b");
    CHECK(route("/x/./y") == "/x/y");

    // an encoded '/' doesn't separate segments.
    CHECK(route("/x%2Fy") == "HTTP/1.1 404");
}

TEST_CASE("url::origin_form is a valid target, as given", "[url][origin_form]")
{
    net::url u;
    u.path  = "/a b/c:d@e/%/%2F";
    u.query = "k=v 1&k=2+3&z";

    CHECK(u.origin_form() == "/a%20b/c:d@e/%25/%2F?k=v%201&k=2+3&z");
    CHECK(net::http::parse_request_target(request_method::GET, u.origin_form()).has_value());

    CHECK(net::url{}.origin_form() == "/");
}
