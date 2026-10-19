#include "http/router.hpp"

#include <exception> // IWYU pragma: keep
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/middleware.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/route_pattern.hpp"
#include "io/string_writer.hpp"
#include "util/type_map.hpp"

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

    return {.status = resp.status_code(), .route = std::string{req.route}, .output = out.build()};
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

TEST_CASE("handlers may take the request as const, or not", "[http][router]")
{
    http::router router;
    router.GET("/const", ok);
    router.GET("/mutable",
               [](http::server_request& req, http::response_writer& resp) -> task<void>
               {
                   req.extensions.emplace<int>(42);
                   co_await resp.send(http::status::OK, 0);
               });

    CHECK(route(router, http::request_method::GET, "/const").status == http::status::OK);
    CHECK(route(router, http::request_method::GET, "/mutable").status == http::status::OK);
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

// middleware on routes, with use().
namespace
{

using steps = std::vector<std::string>;

// recording records its name before and after the rest of the route's chain.
struct recording
{
    std::string name;
    steps*      out;

    template<class Next>
    task<void> operator()(http::server_request& req, http::response_writer& resp, Next& next) const
    {
        out->push_back(name + " before");
        co_await next(req, resp);
        out->push_back(name + " after");
    }
};

// out_of_line is a middleware that takes a next_handler, so that its body can be out of line (as in a .cpp).
struct out_of_line
{
    steps* out;

    task<void> operator()(http::server_request& req, http::response_writer& resp, http::next_handler next) const;
};

task<void>
out_of_line::operator()(http::server_request& req, http::response_writer& resp, http::next_handler next) const
{
    out->emplace_back("out of line before");
    co_await next(req, resp);
    out->emplace_back("out of line after");
}

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
    steps*                       out;

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

TEST_CASE("a subrouter's middleware runs only for its routes", "[http][router][middleware]")
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
            .name   = "the subrouter's route",
            .method = http::request_method::GET,
            .path   = "/api/users/1",
            .status = http::status::OK,
            .calls  = {"api before", "handler", "api after"}
    },
        test_case{.name   = "the subrouter's route, with a method it doesn't have",
                  .method = http::request_method::POST,
                  .path   = "/api/users/1",
                  .status = http::status::METHOD_NOT_ALLOWED,
                  .calls  = {}},
        test_case{.name   = "no route in the subrouter",
                  .method = http::request_method::GET,
                  .path   = "/api/nope",
                  .status = http::status::NOT_FOUND,
                  .calls  = {}},
        test_case{.name   = "the subrouter's prefix",
                  .method = http::request_method::GET,
                  .path   = "/api",
                  .status = http::status::NOT_FOUND,
                  .calls  = {}},
        test_case{.name   = "another subrouter",
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

    http::router router;
    router.GET("/", recorded{&calls})
        .subrouter("/api",
                   [&calls](http::router& api)
                   { api.use(recording{.name = "api", .out = &calls}).GET("/users/:id", recorded{&calls}); })
        .subrouter("/other", [&calls](http::router& other) { other.GET("/x", recorded{&calls}); });

    CHECK(route(router, tc.method, tc.path).status == tc.status);
    CHECK(calls == tc.calls);
}

TEST_CASE("subrouters' middleware runs outermost first, whenever it was added", "[http][router][middleware]")
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
                      "a1 before", "out of line before",
                      "b before", "handler",
                      "b after", "out of line after",
                      "a1 after", "top after"}
    },
        test_case{.path  = "/a/d",
                  .calls = {"top before",
                            "a1 before",
                            "out of line before",
                            "handler",
                            "out of line after",
                            "a1 after",
                            "top after"}},
        // added to the root under /a, outside the subrouter: it's in the subrouter all the same.
        test_case{.path  = "/a/f",
                  .calls = {"top before",
                            "a1 before",
                            "out of line before",
                            "handler",
                            "out of line after",
                            "a1 after",
                            "top after"}},
        test_case{.path = "/e", .calls = {"top before", "handler", "top after"}},
        // the root's use() is for its routes, not for its 404s.
        test_case{.path = "/nope", .calls = {}});
    CAPTURE(tc.path);

    steps calls;

    // every use() follows the routes it applies to.
    http::router router;
    router.GET("/e", recorded{&calls})
        .subrouter("/a",
                   [&calls](http::router& a)
                   {
                       a.subrouter("/b",
                                   [&calls](http::router& b)
                                   { b.GET("/c", recorded{&calls}).use(recording{.name = "b", .out = &calls}); })
                           .GET("/d", recorded{&calls})
                           .use(recording{.name = "a1", .out = &calls}, out_of_line{&calls});
                   })
        .GET("/a/f", recorded{&calls})
        .use(recording{.name = "top", .out = &calls});

    route(router, http::request_method::GET, tc.path);
    CHECK(calls == tc.calls);
}

TEST_CASE("a subrouter's middleware sees the route and its parameters", "[http][router][middleware][path_value]")
{
    steps       calls;
    std::string id;

    http::router router;
    router.subrouter("/orgs/:org",
                     [&](http::router& org)
                     {
                         org.use(seeing{&calls})
                             .GET("/users/:id",
                                  [&](const http::server_request& req, http::response_writer& resp) -> task<void>
                                  {
                                      id = req.path_value("id");
                                      co_await resp.send(http::status::OK, 0);
                                  });
                     });

    CHECK(route(router, http::request_method::GET, "/orgs/acme/users/3").status == http::status::OK);
    CHECK(id == "3");
    CHECK(calls == steps{"/orgs/:org/users/:id acme"});
}

TEST_CASE("a subrouter's middleware may answer without calling the rest", "[http][router][middleware]")
{
    steps calls;

    http::router router;
    router.subrouter(
        "/api",
        [&calls](http::router& api)
        {
            api.use(recording{.name = "outer", .out = &calls}, denying{}, recording{.name = "inner", .out = &calls})
                .GET("/x", recorded{&calls});
        });

    CHECK(route(router, http::request_method::GET, "/api/x").status == http::status::UNAUTHORIZED);
    CHECK(calls == steps{"outer before", "outer after"});
}

TEST_CASE("HEAD handled by GET goes through the subrouter's middleware", "[http][router][middleware]")
{
    steps calls;

    http::router router;
    router.subrouter("/api",
                     [&calls](http::router& api)
                     { api.use(recording{.name = "api", .out = &calls}).GET("/x", recorded{&calls}); });

    CHECK(route(router, http::request_method::HEAD, "/api/x").status == http::status::OK);
    CHECK(calls == steps{"api before", "handler", "api after"});
}

TEST_CASE("a router's middleware may be move-only, and the router moved", "[http][router][middleware]")
{
    steps calls;

    http::router router;
    router.use(move_only{.out = &calls}).GET("/x", recorded{&calls}).GET("/y", recorded{&calls});

    // the routes' chains refer to themselves: moving the router keeps them where they are.
    const auto moved = std::move(router);

    CHECK(route(moved, http::request_method::GET, "/x").status == http::status::OK);
    CHECK(route(moved, http::request_method::GET, "/y").status == http::status::OK);
    CHECK(calls == steps{"move only", "handler", "move only", "handler"});
}
