#include "http/router.hpp"

#include <exception> // IWYU pragma: keep
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/route_pattern.hpp"
#include "io/string_writer.hpp"

namespace http = net::http;

namespace
{

using net::coro::task;

task<void> ok(const http::server_request& /*req*/, http::response_writer& resp)
{
    co_await resp.send(http::status::OK, 0);
}

}

TEST_CASE("the router sets the route that matched", "[http][router]")
{
    struct test_case
    {
        std::string          name;
        http::request_method method;
        std::string          path;
        http::status         expect_status;
        std::string          expect_route;
    };

    auto tc = GENERATE(
        test_case{
            .name          = "root",
            .method        = http::request_method::GET,
            .path          = "/",
            .expect_status = http::status::OK,
            .expect_route  = "/",
        },
        test_case{
            .name          = "wildcard",
            .method        = http::request_method::GET,
            .path          = "/users/42",
            .expect_status = http::status::OK,
            .expect_route  = "/users/:id",
        },
        test_case{
            .name          = "subrouter",
            .method        = http::request_method::GET,
            .path          = "/api/v1/health",
            .expect_status = http::status::OK,
            .expect_route  = "/api/v1/health",
        },
        test_case{
            .name          = "wrong method",
            .method        = http::request_method::POST,
            .path          = "/users/42",
            .expect_status = http::status::METHOD_NOT_ALLOWED,
            .expect_route  = "/users/:id",
        },
        test_case{
            .name          = "no route",
            .method        = http::request_method::GET,
            .path          = "/nope",
            .expect_status = http::status::NOT_FOUND,
            .expect_route  = "",
        },
        test_case{
            .name          = "only part of a route",
            .method        = http::request_method::GET,
            .path          = "/users",
            .expect_status = http::status::NOT_FOUND,
            .expect_route  = "",
        });

    http::router router;
    router.GET("/", ok).GET("/users/:id", ok).subrouter("/api", [](http::router& api) { api.GET("/v1/health", ok); });

    SECTION(tc.name)
    {
        http::server_request req{
            .method  = tc.method,
            .version = {.major = 1, .minor = 1}
        };
        req.target.path = tc.path;

        net::io::string_writer<char> out;
        http::server_response        base{
            .version = {.major = 1, .minor = 1},
            .body    = &out
        };
        http::response_writer resp{&out, &base, http::http11::response_encode};

        test::sync_wait(router(req, resp));

        CHECK(resp.status_code() == tc.expect_status);
        CHECK(req.route == tc.expect_route);
    }
}

namespace
{

struct routed
{
    http::status status = http::status::NONE;
    std::string  route;
    std::string  output; // the response, as encoded
};

// route runs a request for method and path through r.
routed route(const http::router& r, http::request_method method, std::string path)
{
    http::server_request req{
        .method  = method,
        .version = {.major = 1, .minor = 1}
    };
    req.target.path = std::move(path);

    net::io::string_writer<char> out;
    http::server_response        base{
        .version = {.major = 1, .minor = 1},
        .body    = &out
    };
    http::response_writer resp{&out, &base, http::http11::response_encode};

    test::sync_wait(r(req, resp));

    return {.status = resp.status_code(), .route = req.route, .output = out.build()};
}

}

TEST_CASE("handlers get the route's parameters", "[http][router][path_value]")
{
    std::string id;
    std::string post;
    std::string missing = "unset";

    http::router router;
    router.GET("/users/:id/posts/:post",
               [&](const http::server_request& req, http::response_writer& resp) -> task<void>
               {
                   id      = req.path_value("id");
                   post    = req.path_value("post");
                   missing = req.path_value("nope");
                   co_await resp.send(http::status::OK, 0);
               });

    // values are decoded.
    CHECK(route(router, http::request_method::GET, "/users/a%20b/posts/7").status == http::status::OK);
    CHECK(id == "a b");
    CHECK(post == "7");
    CHECK(missing.empty());
}

TEST_CASE("literal segments win over parameters, without backtracking", "[http][router]")
{
    http::router router;
    router.GET("/users/me/settings", ok).GET("/users/:id/posts", ok);

    CHECK(route(router, http::request_method::GET, "/users/me/settings").route == "/users/me/settings");
    CHECK(route(router, http::request_method::GET, "/users/42/posts").route == "/users/:id/posts");

    // "me" is taken as the literal segment, which has no "posts" under it: the parameter isn't tried instead.
    CHECK(route(router, http::request_method::GET, "/users/me/posts").status == http::status::NOT_FOUND);
}

TEST_CASE("parameters don't match empty segments", "[http][router]")
{
    http::router router;
    router.GET("/users/:id", ok);

    CHECK(route(router, http::request_method::GET, "/users/").status == http::status::NOT_FOUND);
    CHECK(route(router, http::request_method::GET, "/users/1").status == http::status::OK);
}

TEST_CASE("405 Method Not Allowed lists the route's methods", "[http][router]")
{
    http::router router;
    router.POST("/items", ok).GET("/items", ok);

    auto res = route(router, http::request_method::DELETE, "/items");
    CHECK(res.status == http::status::METHOD_NOT_ALLOWED);
    // in a fixed order, with HEAD, which GET handles.
    CHECK(res.output.contains("\r\nAllow: GET, HEAD, POST\r\n"));
}

TEST_CASE("HEAD is handled by GET, unless the route has its own", "[http][router]")
{
    std::string called;

    http::router router;
    router
        .GET("/a",
             [&](const http::server_request&, http::response_writer& resp) -> task<void>
             {
                 called = "GET";
                 co_await resp.send(http::status::OK, 0);
             })
        .GET("/b", ok)
        .HEAD("/b",
              [&](const http::server_request&, http::response_writer& resp) -> task<void>
              {
                  called = "HEAD";
                  co_await resp.send(http::status::OK, 0);
              })
        .POST("/c", ok);

    CHECK(route(router, http::request_method::HEAD, "/a").status == http::status::OK);
    CHECK(called == "GET");

    CHECK(route(router, http::request_method::HEAD, "/b").status == http::status::OK);
    CHECK(called == "HEAD");

    CHECK(route(router, http::request_method::HEAD, "/c").status == http::status::METHOD_NOT_ALLOWED);
}

TEST_CASE("routes can be made at runtime, by parsing them", "[http][router][route_pattern]")
{
    const std::string from_config = "/items/:id";

    auto pattern = http::route_pattern::parse(from_config);
    REQUIRE(pattern.has_value());

    http::router router;
    router.GET(*pattern, ok);

    CHECK(route(router, http::request_method::GET, "/items/7").route == "/items/:id");
}
