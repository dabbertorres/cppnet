#include "http/static_router.hpp"

#include <array>
#include <concepts>
#include <exception> // IWYU pragma: keep
#include <memory>
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
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/string_writer.hpp"
#include "util/fixed_string.hpp"
#include "util/type_map.hpp"

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

    // NOTE: copied, as they view the request.
    std::vector<std::pair<std::string, std::string>> values;
    for (const auto& [name, value] : req.path_values) values.emplace_back(name, value);

    return {.status      = resp.status_code(),
            .route       = std::string{req.route},
            .path_values = values,
            .output      = out.build()};
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

    const auto routes = http::make_static_router(http::GET("/users/:id/posts/:post"_route,
                                                           [&](const http::server_request&,
                                                               http::response_writer&                 resp,
                                                               http::params<"/users/:id/posts/:post"> p) -> task<void>
                                                           {
                                                               id   = p.get<"id">();
                                                               post = p.get<"post">();
                                                               co_await resp.send(http::status::OK, 0);
                                                           }));

    CHECK(route(routes, http::request_method::GET, "/users/a%20b/posts/7").status == http::status::OK);
    CHECK(id == "a b");
    CHECK(post == "7");
}

TEST_CASE("static routes' handlers may take the request as const, or not", "[http][static_router]")
{
    const auto routes = http::make_static_router(
        http::GET("/const"_route,
                  [](const http::server_request&, http::response_writer& resp) -> task<void>
                  { co_await resp.send(http::status::OK, 0); }),
        http::GET("/mutable/:id"_route,
                  [](http::server_request& req, http::response_writer& resp, http::params<"/mutable/:id">) -> task<void>
                  {
                      req.extensions.emplace<int>(42);
                      co_await resp.send(http::status::OK, 0);
                  }));

    CHECK(route(routes, http::request_method::GET, "/const").status == http::status::OK);
    CHECK(route(routes, http::request_method::GET, "/mutable/7").status == http::status::OK);
}

TEST_CASE("handlers in a group take their own route's parameters", "[http][static_router][params]")
{
    std::string id;

    const auto handler =
        [&](const http::server_request&, http::response_writer& resp, http::params<"/users/:id"> p) -> task<void>
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

TEST_CASE("more parameters than path_values holds in itself", "[http][static_router][params]")
{
    std::vector<std::string> got;

    const auto routes = http::make_static_router(
        http::GET("/:a/:b/:c/:d/:e/:f"_route,
                  [&](const http::server_request&, http::response_writer& resp, http::params<"/:a/:b/:c/:d/:e/:f"> p)
                      -> task<void>
                  {
                      got = {std::string{p.get<"a">()}, std::string{p.get<"d">()}, std::string{p.get<"f">()}};
                      co_await resp.send(http::status::OK, 0);
                  }));

    // some decoded, some not.
    auto res = route(routes, http::request_method::GET, "/1/2/%33/4/5/six%21");
    CHECK(res.status == http::status::OK);
    CHECK(got == std::vector<std::string>{"1", "4", "six!"});
    CHECK(res.path_values.size() == 6);
    CHECK(res.path_values[2].second == "3");
}

TEST_CASE("decoded parameters stay valid as later ones are decoded", "[http][static_router][params]")
{
    http::router dynamic;
    dynamic.GET("/:a/x/:b/:c", ok);

    const auto fixed = http::make_static_router(http::GET("/:a/x/:b/:c"_route, ok));

    // long enough that the decoded values are on the heap, rather than in the string itself.
    const std::string path = "/first%20value%20is%20long/x/second%20too%20and%20longer/plain";

    for (const auto& res :
         {route(dynamic, http::request_method::GET, path), route(fixed, http::request_method::GET, path)})
    {
        REQUIRE(res.path_values.size() == 3);
        CHECK(res.path_values[0].second == "first value is long");
        CHECK(res.path_values[1].second == "second too and longer");
        CHECK(res.path_values[2].second == "plain");
    }
}

// middleware on routes, with use().
namespace
{

using steps = std::vector<std::string>;

// recording records its name before and after the rest of the route's chain.
struct recording
{
    std::string name;
    steps*      out = nullptr;

    template<class Next>
    task<void> operator()(http::server_request& req, http::response_writer& resp, Next& next) const
    {
        out->push_back(name + " before");
        co_await next(req, resp);
        out->push_back(name + " after");
    }
};

// denying answers 401 Unauthorized itself, without calling the rest of the chain.
struct denying
{
    template<class Next>
    task<void> operator()(http::server_request& /*req*/, http::response_writer& resp, Next& /*next*/) const
    {
        co_await resp.send(http::status::UNAUTHORIZED, 0);
    }
};

// seeing records the route and the "org" parameter, as middleware on a route sees them, and returns the rest's task
// without a coroutine of its own.
struct seeing
{
    steps* out;

    template<class Next>
    task<void> operator()(http::server_request& req, http::response_writer& resp, Next& next) const
    {
        out->push_back(std::string{req.route} + " " + std::string{req.path_value("org")});
        return next(req, resp);
    }
};

// move_only is a middleware that can't be copied.
struct move_only
{
    std::unique_ptr<std::string> name = std::make_unique<std::string>("move only");
    steps*                       out  = nullptr;

    template<class Next>
    task<void> operator()(http::server_request& req, http::response_writer& resp, Next& next) const
    {
        out->push_back(*name);
        return next(req, resp);
    }
};

// recorded is a handler that records that it ran, and answers 200 OK.
struct recorded
{
    steps* out;

    task<void> operator()(const http::server_request& /*req*/, http::response_writer& resp) const
    {
        out->emplace_back("handler");
        co_await resp.send(http::status::OK, 0);
    }
};

}

TEST_CASE("a group's middleware runs only for its routes", "[http][static_router][middleware]")
{
    struct test_case
    {
        std::string          name;
        http::request_method method;
        std::string          path;
        http::status         status;
        steps                calls;
    };

    auto tc = GENERATE(
        test_case{
            .name   = "the group's route",
            .method = http::request_method::GET,
            .path   = "/api/users/1",
            .status = http::status::OK,
            .calls  = {"api before", "handler", "api after"}
    },
        test_case{.name   = "the group's route, with a method it doesn't have",
                  .method = http::request_method::POST,
                  .path   = "/api/users/1",
                  .status = http::status::METHOD_NOT_ALLOWED,
                  .calls  = {}},
        test_case{.name   = "no route in the group",
                  .method = http::request_method::GET,
                  .path   = "/api/nope",
                  .status = http::status::NOT_FOUND,
                  .calls  = {}},
        test_case{.name   = "the group's prefix",
                  .method = http::request_method::GET,
                  .path   = "/api",
                  .status = http::status::NOT_FOUND,
                  .calls  = {}},
        test_case{.name   = "another group",
                  .method = http::request_method::GET,
                  .path   = "/other/x",
                  .status = http::status::OK,
                  .calls  = {"handler"}},
        test_case{.name   = "the root",
                  .method = http::request_method::GET,
                  .path   = "/",
                  .status = http::status::OK,
                  .calls  = {"handler"}});
    CAPTURE(tc.name);

    steps calls;

    const auto routes = http::make_static_router(http::GET("/"_route, recorded{&calls}),
                                                 http::group("/api"_route,
                                                             http::use(recording{.name = "api", .out = &calls}),
                                                             http::GET("/users/:id"_route, recorded{&calls})),
                                                 http::group("/other"_route, http::GET("/x"_route, recorded{&calls})));

    CHECK(route(routes, tc.method, tc.path).status == tc.status);
    CHECK(calls == tc.calls);
}

TEST_CASE("nested groups' middleware runs outermost first", "[http][static_router][middleware]")
{
    struct test_case
    {
        std::string path;
        steps       calls;
    };

    auto tc = GENERATE(
        test_case{
            .path  = "/a/b/c",
            .calls = {"top before",
                      "a1 before", "a2 before",
                      "b before", "handler",
                      "b after", "a2 after",
                      "a1 after", "top after"}
    },
        test_case{.path  = "/a/d",
                  .calls = {"top before", "a1 before", "a2 before", "handler", "a2 after", "a1 after", "top after"}},
        test_case{.path = "/e", .calls = {"top before", "handler", "top after"}},
        // use() in make_static_router is for its routes, not for its 404s.
        test_case{.path = "/nope", .calls = {}});
    CAPTURE(tc.path);

    steps calls;

    const auto routes = http::make_static_router(
        http::use(recording{.name = "top", .out = &calls}),
        http::group("/a"_route,
                    http::use(recording{.name = "a1", .out = &calls}, recording{.name = "a2", .out = &calls}),
                    http::group("/b"_route,
                                http::use(recording{.name = "b", .out = &calls}),
                                http::GET("/c"_route, recorded{&calls})),
                    http::GET("/d"_route, recorded{&calls})),
        http::GET("/e"_route, recorded{&calls}));

    route(routes, http::request_method::GET, tc.path);
    CHECK(calls == tc.calls);
}

TEST_CASE("a group's middleware sees the route, and handlers still get their params",
          "[http][static_router][middleware][params]")
{
    steps       calls;
    std::string id;
    std::string full_id;

    const auto routes = http::make_static_router(http::group(
        "/orgs/:org"_route,
        http::use(seeing{&calls}),
        http::GET(
            "/users/:id"_route,
            [&](const http::server_request&, http::response_writer& resp, http::params<"/users/:id"> p) -> task<void>
            {
                id = p.get<"id">();
                co_await resp.send(http::status::OK, 0);
            }),
        // the params of the route's whole pattern.
        http::GET("/teams/:id"_route,
                  [&](const http::server_request&, http::response_writer& resp, http::params<"/orgs/:org/teams/:id"> p)
                      -> task<void>
                  {
                      full_id = std::string{p.get<"org">()} + "/" + std::string{p.get<"id">()};
                      co_await resp.send(http::status::OK, 0);
                  })));

    CHECK(route(routes, http::request_method::GET, "/orgs/acme/users/3").status == http::status::OK);
    CHECK(id == "3");

    CHECK(route(routes, http::request_method::GET, "/orgs/acme/teams/7").status == http::status::OK);
    CHECK(full_id == "acme/7");

    CHECK(calls == steps{"/orgs/:org/users/:id acme", "/orgs/:org/teams/:id acme"});
}

TEST_CASE("a group's middleware may answer without calling the rest", "[http][static_router][middleware]")
{
    steps calls;

    const auto routes = http::make_static_router(http::group(
        "/api"_route,
        http::use(recording{.name = "outer", .out = &calls}, denying{}, recording{.name = "inner", .out = &calls}),
        http::GET("/x"_route, recorded{&calls})));

    CHECK(route(routes, http::request_method::GET, "/api/x").status == http::status::UNAUTHORIZED);
    CHECK(calls == steps{"outer before", "outer after"});
}

TEST_CASE("HEAD handled by GET goes through the group's middleware", "[http][static_router][middleware]")
{
    steps calls;

    const auto routes = http::make_static_router(http::group("/api"_route,
                                                             http::use(recording{.name = "api", .out = &calls}),
                                                             http::GET("/x"_route, recorded{&calls})));

    CHECK(route(routes, http::request_method::HEAD, "/api/x").status == http::status::OK);
    CHECK(calls == steps{"api before", "handler", "api after"});
}

TEST_CASE("middleware that can't be copied can be used on one route", "[http][static_router][middleware]")
{
    steps calls;

    auto routes = http::make_static_router(
        http::group("/api"_route, http::use(move_only{.out = &calls}), http::GET("/x"_route, recorded{&calls})));

    // and the router can still be moved, as into a server.
    const auto moved = std::move(routes);

    CHECK(route(moved, http::request_method::GET, "/api/x").status == http::status::OK);
    CHECK(calls == steps{"move only", "handler"});
}

// a router whose routes have middleware is copyable if its middleware and handlers are.
static_assert(std::copy_constructible<decltype(http::make_static_router(http::group(
                  "/api"_route, http::use(denying{}), http::GET("/x"_route, ok), http::GET("/y"_route, ok))))>);

// handlers take their params by value: a reference would outlive them. (A generic lambda can't be checked.)
namespace
{

using by_value = decltype([](const http::server_request&, http::response_writer&, http::params<"/:id">) -> task<void>
                          { co_return; });
using by_reference =
    decltype([](const http::server_request&, http::response_writer&, const http::params<"/:id">&) -> task<void>
             { co_return; });
using generic = decltype([](const http::server_request&, http::response_writer&, auto) -> task<void> { co_return; });

}

static_assert(!http::detail::takes_params_by_reference<by_value>);
static_assert(http::detail::takes_params_by_reference<by_reference>);
static_assert(!http::detail::takes_params_by_reference<generic>);

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
