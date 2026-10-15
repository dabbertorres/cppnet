#include "http/static_router.hpp"

#include <array>
#include <exception> // IWYU pragma: keep
#include <string>
#include <string_view>
#include <utility>
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
#include "util/fixed_string.hpp"

namespace http = net::http;

using http::literals::operator""_route;

namespace
{

using net::coro::task;

task<void> ok(const http::server_request& /*req*/, http::response_writer& resp)
{
    co_await resp.send(http::status::OK, 0);
}

struct routed
{
    http::status                                     status = http::status::NONE;
    std::string                                      route;
    std::vector<std::pair<std::string, std::string>> path_values;
    std::string                                      output; // the response, as encoded
};

// route runs a request for method and path through r.
template<typename Router>
routed route(const Router& r, http::request_method method, std::string path)
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

    return {.status = resp.status_code(), .route = req.route, .path_values = req.path_values, .output = out.build()};
}

}

TEST_CASE("the static router routes as the runtime router does", "[http][static_router]")
{
    http::router dynamic;
    dynamic.GET("/", ok)
        .GET("/users/:id", ok)
        .POST("/users/:id", ok)
        .GET("/users/me/settings", ok)
        .GET("/users/:id/posts/:post", ok)
        .GET("/x/y", ok)
        .GET("/x/a b", ok)
        .HEAD("/own-head", ok)
        .GET("/own-head", ok)
        .subrouter("/api", [](http::router& api) { api.GET("/v1/health", ok).GET("/", ok); });

    const auto fixed = http::make_static_router(
        http::GET("/"_route, ok),
        http::GET("/users/:id"_route, ok),
        http::POST("/users/:id"_route, ok),
        http::GET("/users/me/settings"_route, ok),
        http::GET("/users/:id/posts/:post"_route, ok),
        http::GET("/x/y"_route, ok),
        http::GET("/x/a b"_route, ok),
        http::HEAD("/own-head"_route, ok),
        http::GET("/own-head"_route, ok),
        http::group("/api"_route, http::GET("/v1/health"_route, ok), http::GET("/"_route, ok)));

    struct test_case
    {
        http::request_method method;
        std::string          path;
    };

    auto tc = GENERATE(test_case{.method = http::request_method::GET, .path = "/"},
                       test_case{.method = http::request_method::GET, .path = "/users/42"},
                       test_case{.method = http::request_method::POST, .path = "/users/42"},
                       test_case{.method = http::request_method::DELETE, .path = "/users/42"},
                       test_case{.method = http::request_method::HEAD, .path = "/users/42"},
                       test_case{.method = http::request_method::NONE, .path = "/users/42"},
                       test_case{.method = http::request_method::GET, .path = "/users/me/settings"},
                       test_case{.method = http::request_method::GET, .path = "/users/me/posts/1"},
                       test_case{.method = http::request_method::GET, .path = "/users/7/posts/a%2Fb"},
                       test_case{.method = http::request_method::GET, .path = "/users/"},
                       test_case{.method = http::request_method::GET, .path = "/users"},
                       test_case{.method = http::request_method::GET, .path = "/x/y"},
                       test_case{.method = http::request_method::GET, .path = "/x/%79"},
                       test_case{.method = http::request_method::GET, .path = "/x/a%20b"},
                       test_case{.method = http::request_method::GET, .path = "/x%2Fy"},
                       test_case{.method = http::request_method::GET, .path = "/x/%7"},
                       test_case{.method = http::request_method::HEAD, .path = "/own-head"},
                       test_case{.method = http::request_method::POST, .path = "/own-head"},
                       test_case{.method = http::request_method::GET, .path = "/api/v1/health"},
                       test_case{.method = http::request_method::GET, .path = "/api/"},
                       test_case{.method = http::request_method::GET, .path = "/api"},
                       test_case{.method = http::request_method::GET, .path = "/nope"},
                       test_case{.method = http::request_method::GET, .path = "/users/1/posts/2/extra"});
    CAPTURE(http::method_string(tc.method), tc.path);

    const auto want = route(dynamic, tc.method, tc.path);
    const auto got  = route(fixed, tc.method, tc.path);

    CHECK(got.status == want.status);
    CHECK(got.route == want.route);
    CHECK(got.path_values == want.path_values);
    CHECK(got.output == want.output);
}

TEST_CASE("static routes' handlers get their parameters by name", "[http][static_router][params]")
{
    std::string id;
    std::string post;

    const auto routes =
        http::make_static_router(http::GET("/users/:id/posts/:post"_route,
                                           [&](const http::server_request&,
                                               http::response_writer&                        resp,
                                               const http::params<"/users/:id/posts/:post">& p) -> task<void>
                                           {
                                               id   = p.get<"id">();
                                               post = p.get<"post">();
                                               co_await resp.send(http::status::OK, 0);
                                           }));

    CHECK(route(routes, http::request_method::GET, "/users/a%20b/posts/7").status == http::status::OK);
    CHECK(id == "a b");
    CHECK(post == "7");
}

TEST_CASE("handlers in a group take their own route's parameters", "[http][static_router][params]")
{
    std::string id;

    const auto handler =
        [&](const http::server_request&, http::response_writer& resp, const http::params<"/users/:id">& p) -> task<void>
    {
        id = p.get<"id">();
        co_await resp.send(http::status::OK, 0);
    };

    const auto routes =
        http::make_static_router(http::group("/orgs/:org"_route, http::GET("/users/:id"_route, handler)));

    auto res = route(routes, http::request_method::GET, "/orgs/acme/users/3");
    CHECK(res.status == http::status::OK);
    CHECK(res.route == "/orgs/:org/users/:id");
    CHECK(id == "3");

    // the group's parameters are still in the request.
    CHECK(res.path_values.front().second == "acme");
}

// params only has the names its pattern does.
template<typename P, net::util::fixed_string Name>
concept has_param = requires(const P& p) { p.template get<Name>(); };

static_assert(has_param<http::params<"/users/:id">, "id">);
static_assert(!has_param<http::params<"/users/:id">, "idd">);
static_assert(!has_param<http::params<"/users/id">, "id">);

// routes are checked against each other at compile time.
namespace
{

using http::detail::check_routes;
using http::detail::route_spec;
using http::detail::routes_error;

constexpr auto get(std::string_view pattern)
{
    return route_spec{.method = http::request_method::GET, .pattern = pattern};
}
constexpr auto post(std::string_view pattern)
{
    return route_spec{.method = http::request_method::POST, .pattern = pattern};
}

}

static_assert(check_routes(std::array{get("/a"), post("/a"), get("/a/:x"), get("/a/:x/b")}) == routes_error::none);
static_assert(check_routes(std::array{get("/a"), get("/a")}) == routes_error::duplicate_route);
static_assert(check_routes(std::array{get("/a/:x"), post("/a/:y")}) == routes_error::conflicting_parameter);
static_assert(check_routes(std::array{get("/a/:x/b"), get("/a/:y/c")}) == routes_error::conflicting_parameter);
