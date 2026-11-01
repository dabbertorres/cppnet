// Reprioritizing a request in flight: client_response::set_priority() sends the server a PRIORITY_UPDATE for the
// response's stream, over HTTP/2, which cppnet's server takes as the stream's priority (read by its handler with
// detail::stream_priority()); over HTTP/1.1, and once the response has ended, it can't.
#include <cstddef>
#include <cstdint>
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

    // what the client saw: what went wrong, if anything, how many updates set_priority() took, /ping's body, and the
    // held response's body.
    struct seen
    {
        std::string failed;
        std::size_t taken = 0;
        std::string pong;
        std::string body;
    };

    const auto test = [](served* server, http::client* client, const test_case* tc) -> task<seen>
    {
        // NOTE: the gate's opened whatever happens, so the handler finishes.
        net::util::defer opening{[server] noexcept { server->held.open(); }};

        seen out;
        auto resp = co_await client->send(get(server->url("/hold"), tc->field));
        if (!resp.has_value())
        {
            out.failed = "/hold: " + resp.error().message();
            co_return out;
        }
        for (const auto& p : tc->updates) out.taken += resp->set_priority(p) ? 1 : 0;

        // once /ping's answered (on the same connection, after the updates), the server has them.
        auto ping = co_await client->send(get(server->url("/ping")));
        if (!ping.has_value())
        {
            out.failed = "/ping: " + ping.error().message();
            co_return out;
        }
        out.pong = co_await body_of(*ping);

        server->held.open();
        out.body = co_await body_of(*resp);
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&server, &client, &tc));
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());
    CHECK(out.taken == tc.updates.size());
    CHECK(out.pong == "pong");
    CHECK(out.body == tc.want);
}

TEST_CASE("set_priority() can't reprioritize a response that's ended, failed or isn't the client's, nor over HTTP/1.1",
          "[http][http2][client][priority]")
{
    net::io::scheduler sched{make_pool()};
    served             server{&sched};

    SECTION("over HTTP/1.1")
    {
        // what the client saw: what went wrong, if anything, the response's version, whether set_priority() took it,
        // and the body.
        struct seen
        {
            std::string   failed;
            std::uint32_t version = 0;
            bool          taken   = false;
            std::string   body;
        };

        http::client client{&sched, options_for(false)};
        const auto   test = [](served* server, http::client* client) -> task<seen>
        {
            seen out;
            auto resp = co_await client->send(get(server->url("/small")));
            if (!resp.has_value())
            {
                out.failed = resp.error().message();
                co_return out;
            }
            out.version = resp->version.major;
            out.taken   = resp->set_priority({.urgency = 1});
            out.body    = co_await body_of(*resp);
            co_return out;
        };

        const auto out = sched.run_to_completion(test(&server, &client));
        CAPTURE(out.failed);
        REQUIRE(out.failed.empty());
        CHECK(out.version == 1U);
        CHECK_FALSE(out.taken);
        CHECK(out.body == "x");
    }

    SECTION("over HTTP/2")
    {
        // what the client saw: what went wrong, if anything, and whether set_priority() took each update.
        struct seen
        {
            std::string   failed;
            std::uint32_t version = 0;
            std::string   body;                    // the ended response's
            bool          ended           = true;  // taken for the response that's ended
            bool          before_canceled = false; // taken for the one canceled, before
            bool          after_canceled  = true;  // and after
            bool          replaced        = true;  // taken for the one whose body was replaced
            bool          none            = true;  // taken for no response at all
        };

        http::client client{&sched, options_for(true)};
        const auto   test = [](served* server, http::client* client) -> task<seen>
        {
            net::util::defer opening{[server] noexcept { server->held.open(); }};
            seen             out;

            // its body read to its end: the server's sent all of it.
            auto ended = co_await client->send(get(server->url("/small")));
            if (!ended.has_value())
            {
                out.failed = "/small: " + ended.error().message();
                co_return out;
            }
            out.version = ended->version.major;
            out.body    = co_await body_of(*ended);
            out.ended   = ended->set_priority({.urgency = 1});

            // canceled: its stream's failed.
            std::stop_source stop;
            auto             req = get(server->url("/hold"));
            req.stop             = stop.get_token();
            auto canceled        = co_await client->send(std::move(req));
            if (!canceled.has_value())
            {
                out.failed = "/hold: " + canceled.error().message();
                co_return out;
            }
            out.before_canceled = canceled->set_priority({.urgency = 1});
            stop.request_stop();
            out.after_canceled = canceled->set_priority({.urgency = 2});

            // a body the caller put in place of the client's.
            auto replaced = co_await client->send(get(server->url("/small")));
            if (!replaced.has_value())
            {
                out.failed = "/small again: " + replaced.error().message();
                co_return out;
            }
            replaced->body = std::make_unique<net::io::owned_string_reader>("y");
            out.replaced   = replaced->set_priority({.urgency = 1});

            // none at all.
            http::client_response none;
            out.none = none.set_priority({.urgency = 1});
            co_return out;
        };

        const auto out = sched.run_to_completion(test(&server, &client));
        CAPTURE(out.failed);
        REQUIRE(out.failed.empty());
        CHECK(out.version == 2U);
        CHECK(out.body == "x");
        CHECK_FALSE(out.ended);
        CHECK(out.before_canceled);
        CHECK_FALSE(out.after_canceled);
        CHECK_FALSE(out.replaced);
        CHECK_FALSE(out.none);
    }
}
