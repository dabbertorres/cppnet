// Reprioritizing a request in flight (docs/plans/http2.md, stage 6, part 9): client_response::set_priority() sends the
// server a PRIORITY_UPDATE for the response's stream, over HTTP/2, which cppnet's server takes as the stream's priority
// (read by its handler with detail::stream_priority()); over HTTP/1.1, and once the response has ended, it can't.
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/h2_client.hpp"
#include "http/http2_server.hpp"
#include "http/priority.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/string_reader.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "url.hpp"
#include "util/defer.hpp"

namespace
{

namespace http = net::http;

using namespace std::string_view_literals;
using net::coro::task;

// describe is a stream's priority as the handler reports it: "u=3", "u=3 i", or "none" (not HTTP/2).
std::string describe(std::optional<http::priority> p)
{
    if (!p.has_value()) return "none";
    return std::format("u={}{}", p->urgency, p->incremental ? " i" : "");
}

// served is cppnet's server on 127.0.0.1: /hold reports its stream's priority as it starts, sends its head, waits at
// the gate, then reports it again ("start=...;after=..."); /ping answers "pong" (once it has, the server has had what
// the client sent before its request); /small answers "x".
struct served
{
    explicit served(net::io::scheduler* sched)
        : held{sched}
        , server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), routes()}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    [[nodiscard]] std::string url(std::string_view path) const
    {
        return std::format("http://127.0.0.1:{}{}", server.local_port(), path);
    }

    http::router routes()
    {
        http::router r;
        r.GET("/hold",
              [this](const http::server_request& req, http::response_writer& resp) -> task<void>
              {
                  const auto start = describe(http::detail::stream_priority(req));
                  auto*      body  = co_await resp.send(http::status::OK);
                  co_await held.pass();
                  const auto after = describe(http::detail::stream_priority(req));
                  co_await net::io::write_all(*body, std::format("start={};after={}", start, after));
              });
        r.GET("/ping",
              [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
              {
                  auto* body = co_await resp.send(http::status::OK, 4);
                  co_await net::io::write_all(*body, "pong"sv);
              });
        r.GET("/small",
              [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
              {
                  auto* body = co_await resp.send(http::status::OK, 1);
                  co_await net::io::write_all(*body, "x"sv);
              });
        return r;
    }

    test::gate   held;
    http::server server;
};

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

http::client_options options_for(bool http2)
{
    http::client_options options;
    options.http2_prior_knowledge = http2;
    return options;
}

http::client_request get(std::string_view url, std::string_view priority_field = {})
{
    http::client_request req{.method = http::request_method::GET, .uri = net::url::parse(url).value()};
    if (!priority_field.empty()) req.headers.set("Priority", priority_field);
    return req;
}

// body_of reads a response's body to its end ("(error)" if it fails).
task<std::string> body_of(http::client_response& resp)
{
    auto body = co_await net::io::read_all(*resp.body);
    co_return body.has_value() ? std::move(*body) : "(error)";
}

}

TEST_CASE("set_priority() reprioritizes an HTTP/2 response in flight, as the server sees it",
          "[http][http2][client][priority]")
{
    struct test_case
    {
        std::string_view            what;
        std::string_view            field; // the request's Priority field, if any
        std::vector<http::priority> updates;
        std::string_view            want;
    };

    const auto tc = GENERATE(
        test_case{
            .what    = "from the request's field",
            .field   = "u=5",
            .updates = {{.urgency = 1, .incremental = true}},
            .want    = "start=u=5;after=u=1 i"
    },
        test_case{.what    = "repeated, and the last of several",
                  .field   = "u=5",
                  .updates = {{.urgency = 1}, {.urgency = 1}, {.urgency = 6}, {.urgency = 2, .incremental = true}},
                  .want    = "start=u=5;after=u=2 i"},
        // without a field (and the client not yet known to signal, so u=3 incremental), the first update is sent,
        // the default too: it's a signal the request didn't make.
        test_case{.what    = "without a field, to the default",
                  .field   = "",
                  .updates = {{}},
                  .want    = "start=u=3 i;after=u=3"},
        // an urgency past 7 is 7.
        test_case{.what = "past 7", .field = "", .updates = {{.urgency = 9}}, .want = "start=u=3 i;after=u=7"});
    CAPTURE(tc.what);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(true)};

    const auto test = [](served* server, http::client* client, const test_case* tc) -> task<std::string>
    {
        // NOTE: the gate's opened whatever happens, so the handler finishes.
        net::util::defer opening{[server] noexcept { server->held.open(); }};

        auto resp = co_await client->send(get(server->url("/hold"), tc->field));
        REQUIRE(resp.has_value());
        for (const auto& p : tc->updates) CHECK(resp->set_priority(p));

        // once /ping's answered (on the same connection, after the updates), the server has them.
        auto ping = co_await client->send(get(server->url("/ping")));
        REQUIRE(ping.has_value());
        const auto pong = co_await body_of(*ping);
        CHECK(pong == "pong");

        server->held.open();
        co_return co_await body_of(*resp);
    };

    CHECK(sched.run_to_completion(test(&server, &client, &tc)) == tc.want);
}

TEST_CASE("set_priority() can't reprioritize a response that's ended, failed or isn't the client's, nor over HTTP/1.1",
          "[http][http2][client][priority]")
{
    net::io::scheduler sched{make_pool()};
    served             server{&sched};

    SECTION("over HTTP/1.1")
    {
        http::client client{&sched, options_for(false)};
        const auto   test = [](served* server, http::client* client) -> task<>
        {
            auto resp = co_await client->send(get(server->url("/small")));
            REQUIRE(resp.has_value());
            CHECK(resp->version.major == 1);
            CHECK_FALSE(resp->set_priority({.urgency = 1}));
            const auto body = co_await body_of(*resp);
            CHECK(body == "x");
        };
        sched.run_to_completion(test(&server, &client));
    }

    SECTION("over HTTP/2")
    {
        http::client client{&sched, options_for(true)};
        const auto   test = [](served* server, http::client* client) -> task<>
        {
            net::util::defer opening{[server] noexcept { server->held.open(); }};

            // its body read to its end: the server's sent all of it.
            auto ended = co_await client->send(get(server->url("/small")));
            REQUIRE(ended.has_value());
            CHECK(ended->version.major == 2);
            const auto body = co_await body_of(*ended);
            CHECK(body == "x");
            CHECK_FALSE(ended->set_priority({.urgency = 1}));

            // canceled: its stream's failed.
            std::stop_source stop;
            auto             req = get(server->url("/hold"));
            req.stop             = stop.get_token();
            auto canceled        = co_await client->send(std::move(req));
            REQUIRE(canceled.has_value());
            CHECK(canceled->set_priority({.urgency = 1}));
            stop.request_stop();
            CHECK_FALSE(canceled->set_priority({.urgency = 2}));

            // a body the caller put in place of the client's.
            auto replaced = co_await client->send(get(server->url("/small")));
            REQUIRE(replaced.has_value());
            replaced->body = std::make_unique<net::io::owned_string_reader>("y");
            CHECK_FALSE(replaced->set_priority({.urgency = 1}));

            // none at all.
            http::client_response none;
            CHECK_FALSE(none.set_priority({.urgency = 1}));
        };
        sched.run_to_completion(test(&server, &client));
    }
}
