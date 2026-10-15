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
