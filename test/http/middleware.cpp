#include "http/middleware.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception> // IWYU pragma: keep
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "instrument/middleware.hpp"
#include "io/scheduler.hpp"
#include "io/string_writer.hpp"
#include "listen.hpp"

namespace http = net::http;

namespace
{

using namespace std::string_view_literals;

using net::coro::task;

// steps records what the layers and handlers do, in order.
using steps = std::vector<std::string>;

// recording records its name before and after the rest of the chain.
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

// user is what tagging puts in the request's extensions.
struct user
{
    std::string name;
};

// tagging puts a user in the request's extensions, and returns the rest of the chain's task, without a coroutine of its
// own.
struct tagging
{
    template<class Next>
    task<void> operator()(http::server_request& req, http::response_writer& resp, Next& next) const
    {
        req.extensions.emplace<user>("alice");
        return next(req, resp);
    }
};

// stopping swaps req.stop for a token of its own, already stopped, for the rest of the chain, and puts the request's
// back after.
struct stopping
{
    template<class Next>
    task<void> operator()(http::server_request& req, http::response_writer& resp, Next& next) const
    {
        std::stop_source source;
        source.request_stop();

        auto original = std::exchange(req.stop, source.get_token());
        co_await next(req, resp);
        req.stop = std::move(original);
    }
};

// move_only is a middleware that can't be copied.
struct move_only
{
    std::unique_ptr<std::string> header = std::make_unique<std::string>("X-Middleware");

    template<class Next>
    task<void> operator()(http::server_request& req, http::response_writer& resp, Next& next) const
    {
        resp.headers().set(*header, "yes"sv);
        co_await next(req, resp);
    }
};

// handler records that it ran, and answers 200 OK with what it saw: the user in the request's extensions, if there is
// one, and whether its token was stopped.
struct handler
{
    steps* out;

    task<void> operator()(http::server_request& req, http::response_writer& resp) const
    {
        out->emplace_back("handler");

        const auto* u    = req.extensions.get<user>();
        std::string body = u != nullptr ? u->name : "";
        if (req.stop.stop_requested()) body += " stopped";

        auto* writer = co_await resp.send(http::status::OK, body.size());
        co_await net::io::write_all(*writer, body);
    }
};

// throwing throws.
task<void> throwing(const http::server_request& /*req*/, http::response_writer& /*resp*/)
{
    throw std::runtime_error{"oops"};
    co_return;
}

// result is what call got: the response, and whether the chain threw.
struct result
{
    std::string response;
    bool        threw = false;
};

// call calls handler with a GET of /, with stop as the request's token.
template<typename Handler>
result call(const Handler& handler, std::stop_token stop = {})
{
    http::server_request req{
        .method  = http::request_method::GET,
        .version = {.major = 1, .minor = 1}
    };
    req.target.path = "/";
    req.stop        = stop;

    net::io::string_writer<char> out;
    http::server_response        base{
        .version = {.major = 1, .minor = 1},
        .body    = &out
    };
    http::response_writer resp{&out, &base, http::http11::response_encode};

    result res;
    try
    {
        test::sync_wait(handler(req, resp));
    }
    catch (const std::runtime_error&)
    {
        res.threw = true;
    }

    // the token is the request's again, whatever the layers did with it.
    CHECK(req.stop == stop);

    res.response = out.build();
    return res;
}

// get_hello requests /hello from the server on port, returning its X-Middleware header and its body, after a space.
task<std::string> get_hello(net::io::scheduler* sched, std::uint16_t port)
{
    http::client client{sched};

    auto resp = co_await client.get("http://127.0.0.1:" + std::to_string(port) + "/hello");
    if (!resp.has_value()) co_return "error: " + resp.error().message();

    std::string out = std::string{resp->headers.get("X-Middleware"sv).value_or("")} + ' ';

    std::array<std::byte, 64> buf{};
    while (true)
    {
        auto read = co_await resp->body->read(buf);
        out.append(reinterpret_cast<const char*>(buf.data()), read.count);
        if (read.err) break;
    }

    co_return out;
}

// chain_with_router is a chain around a router, which can't be copied, as the guide's example gives a server.
using chain_with_router = decltype(http::chain(net::instrument::http_metrics{}, std::declval<http::router>()));

static_assert(sizeof(http::next_handler) == 2 * sizeof(void*));
static_assert(http::middleware<recording> && http::middleware<out_of_line> && http::middleware<tagging>);
static_assert(!http::middleware<handler>);
static_assert(std::copy_constructible<decltype(http::chain(recording{}, tagging{}, handler{}))>);
static_assert(!std::copy_constructible<chain_with_router> && std::move_constructible<chain_with_router>);
static_assert(std::constructible_from<http::server, net::io::scheduler*, net::listener, chain_with_router>);
static_assert(std::constructible_from<http::server, net::io::scheduler*, chain_with_router>);

}

TEST_CASE("a chain runs its middleware outermost first, then its handler", "[http][middleware][chain]")
{
    steps calls;

    const auto chain = http::chain(recording{.name = "a", .out = &calls},
                                   out_of_line{&calls},
                                   recording{.name = "b", .out = &calls},
                                   handler{&calls});

    auto res = call(chain);
    CHECK_FALSE(res.threw);
    CHECK(res.response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(calls
          == steps{"a before", "out of line before", "b before", "handler", "b after", "out of line after", "a after"});

    SECTION("and its copies do the same")
    {
        calls.clear();

        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization): a copy, to call.
        const auto copy = chain;
        res             = call(copy);
        CHECK(calls.size() == 7);
    }
}

TEST_CASE("a chain may be only a handler", "[http][middleware][chain]")
{
    steps calls;

    auto res = call(http::chain(handler{&calls}));
    CHECK(res.response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(calls == steps{"handler"});
}

TEST_CASE("a middleware may answer without calling the rest of the chain", "[http][middleware][chain]")
{
    steps calls;

    auto res = call(http::chain(recording{.name = "a", .out = &calls},
                                denying{},
                                recording{.name = "b", .out = &calls},
                                handler{&calls}));
    CHECK_FALSE(res.threw);
    CHECK(res.response.starts_with("HTTP/1.1 401 Unauthorized\r\n"));
    CHECK(calls == steps{"a before", "a after"});
}

TEST_CASE("a middleware may return the rest of the chain's task, without a coroutine", "[http][middleware][chain]")
{
    steps calls;

    auto res = call(http::chain(recording{.name = "a", .out = &calls}, tagging{}, handler{&calls}));
    CHECK(res.response.ends_with("\r\n\r\nalice"));
    CHECK(calls == steps{"a before", "handler", "a after"});
}

TEST_CASE("exceptions pass through a chain", "[http][middleware][chain]")
{
    steps calls;

    auto res = call(http::chain(recording{.name = "a", .out = &calls}, out_of_line{&calls}, throwing));
    CHECK(res.threw);

    // a layer that doesn't catch it doesn't go on.
    CHECK(calls == steps{"a before", "out of line before"});
}

TEST_CASE("a middleware may swap the request's token for the rest of the chain", "[http][middleware][chain]")
{
    steps calls;

    // call checks that the request's own token is back.
    std::stop_source source;
    auto             res = call(http::chain(stopping{}, handler{&calls}), source.get_token());
    CHECK(res.response.ends_with("\r\n\r\n stopped"));
    CHECK_FALSE(source.stop_requested());
}

TEST_CASE("a server takes a chain that can't be copied", "[http][middleware][chain][server]")
{
    // NOTE: before the scheduler, as the server's handler uses it.
    steps calls;

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // a router can't be copied, and nor can move_only.
    http::router router;
    router.GET("/hello", handler{&calls});

    http::server server{&sched, std::move(*listener), http::chain(move_only{}, std::move(router))};
    REQUIRE(sched.spawn(server.serve()));

    auto got = sched.run_to_completion(get_hello(&sched, server.local_port()));
    CHECK(got == "yes ");
    CHECK(calls == steps{"handler"});
}
