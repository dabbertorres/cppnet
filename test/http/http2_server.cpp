#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/h2_client.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/http2.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::gate;
using test::h2_client;
using test::header;

namespace
{

task<void> hello(const http::server_request& req, http::response_writer& resp)
{
    const auto msg = std::format("hello over HTTP/{}", req.version.major);
    resp.headers().set("Content-Type", "text/plain");
    auto* body = co_await resp.send(http::status::OK, msg.size());
    co_await net::io::write_all(*body, msg);
}

// echo sends back the request's body, without a Content-Length.
task<void> echo(http::server_request& req, http::response_writer& resp)
{
    auto body = co_await net::io::read_all(*req.body);
    if (!body.has_value()) throw std::system_error{body.error()};

    auto* out = co_await resp.send(http::status::OK);
    co_await net::io::write_all(*out, *body);
}

http::router make_router(net::io::scheduler* sched)
{
    http::router r;
    r.GET("/", hello);
    r.POST("/echo", echo);
    r.GET("/slow",
          [sched](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              (void)co_await sched->sleep_until(net::io::clock::now() + 300ms);
              auto* body = co_await resp.send(http::status::OK, 4);
              co_await net::io::write_all(*body, "slow"sv);
          });
    r.GET("/throw",
          [](const http::server_request&, http::response_writer&) -> task<void> { throw std::runtime_error{"oops"}; });
    r.GET("/throw-mid-body",
          [](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              auto* body = co_await resp.send(http::status::OK, 100);
              co_await net::io::write_all(*body, "partial"sv);
              throw std::runtime_error{"oops"};
          });
    r.GET("/throw-mid-stream",
          [](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              auto* body = co_await resp.send(http::status::OK);
              co_await net::io::write_all(*body, "partial"sv);
              throw std::runtime_error{"oops"};
          });
    r.GET("/throw-after-interim",
          [](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              resp.headers().set("X-Meant-For-200", "yes");
              co_await resp.send(http::status::PROCESSING);
              throw std::runtime_error{"oops"};
          });
    r.GET("/bad-header",
          [](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              // a value with a line break can't be sent, over HTTP/2 as over HTTP/1.1.
              std::string outcome = "sent";
              resp.headers().set("X-Bad", "a\r\nb");
              try
              {
                  co_await resp.send(http::status::OK, 0);
              }
              catch (const std::system_error& e)
              {
                  outcome = e.code() == std::errc::invalid_argument ? "threw" : e.what();
              }

              resp.headers().remove("X-Bad");
              auto* body = co_await resp.send(http::status::OK, outcome.size());
              co_await net::io::write_all(*body, outcome);
          });
    return r;
}

struct test_server
{
    explicit test_server(net::io::scheduler* sched, const http::server_config& cfg = {})
        : test_server{sched, make_router(sched), cfg}
    {}

    test_server(net::io::scheduler* sched, http::router router, const http::server_config& cfg = {})
        : server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), std::move(router), cfg}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    [[nodiscard]] std::string port() const { return std::to_string(server.local_port()); }

    http::server server;
};

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// with_client connects to port, and runs test with an h2_client over the connection (whose reads fail after
// read_timeout without progress, if it's given: for a test whose server could stop sending).
template<typename Test>
task<> with_client(net::io::scheduler*                     sched,
                   std::string                             port,
                   Test                                    test,
                   std::optional<net::io::clock::duration> read_timeout = std::nullopt)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
    REQUIRE(conn.has_value());
    conn->set_read_timeout(read_timeout);

    h2_client client{&*conn, &*conn, &*conn};
    co_await test(client);
}

}

TEST_CASE("HTTP/2 with prior knowledge: a request and its response", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const auto id = c.request("GET", "/");
                                            REQUIRE(co_await c.wait_for(id));

                                            const auto& r = c.responses[id];
                                            CHECK(r.status == "200");
                                            CHECK(r.body == "hello over HTTP/2");
                                            CHECK(header(r, "content-type") == "text/plain");
                                            CHECK(header(r, "content-length") == "17");
                                            CHECK(header(r, "date") != "(none)");
                                        }));
}

TEST_CASE("HTTP/2 bodies larger than the windows go both ways", "[http][http2][server][flow]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    std::string body(300'000, 'x');
    for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>('a' + (i % 26));

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&body](h2_client& c) -> task<>
                                        {
                                            const auto id = c.request("POST", "/echo", body);
                                            REQUIRE(co_await c.wait_for(id));

                                            const auto& r = c.responses[id];
                                            CAPTURE(r.reset.message());
                                            CHECK(r.status == "200");
                                            CHECK(r.body.size() == body.size());
                                            CHECK(r.body == body);
                                        }));
}

TEST_CASE("HTTP/2 streams are served at once: a slow one doesn't hold up the others", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    sched.run_to_completion(
        with_client(&sched,
                    server.port(),
                    [](h2_client& c) -> task<>
                    {
                        const auto                    slow = c.request("GET", "/slow");
                        std::vector<http2::stream_id> fast;
                        fast.reserve(10);
                        for (int i = 0; i < 10; ++i) fast.push_back(c.request("GET", "/"));

                        REQUIRE(co_await c.pump(
                            [&]
                            {
                                if (!c.responses[slow].done) return false;
                                return std::ranges::all_of(fast, [&](auto id) { return c.responses[id].done; });
                            }));

                        // the slow one finished last.
                        CHECK(c.responses[slow].body == "slow");
                        CHECK(c.responses[slow].order == 11);
                        for (const auto id : fast) CHECK(c.responses[id].body == "hello over HTTP/2");
                    }));
}

TEST_CASE("HTTP/2 handlers that throw, HEAD, and unknown methods", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const auto thrown  = c.request("GET", "/throw");
                                            const auto head    = c.request("HEAD", "/");
                                            const auto unknown = c.request("BREW", "/");
                                            REQUIRE(co_await c.pump([&] { return c.finished == 3; }));

                                            CHECK(c.responses[thrown].status == "500");
                                            CHECK(c.responses[head].status == "200");
                                            CHECK(header(c.responses[head], "content-length") == "17");
                                            CHECK(c.responses[head].body.empty());
                                            CHECK(c.responses[unknown].status == "501");
                                            CHECK(header(c.responses[unknown], "date") != "(none)");
                                        }));
}

TEST_CASE("an HTTP/2 stream whose body stops arriving is reset alone", "[http][http2][server][timeouts]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, {.body_read_timeout = 1s}};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            // a request whose body never comes.
                                            const auto stalled = c.request("POST", "/echo", "", false);
                                            REQUIRE(co_await c.wait_for(stalled));
                                            CHECK(c.responses[stalled].reset == http2::error_code::cancel);

                                            // the connection goes on.
                                            const auto next = c.request("GET", "/");
                                            REQUIRE(co_await c.wait_for(next));
                                            CHECK(c.responses[next].status == "200");
                                        }));
}

TEST_CASE("an idle HTTP/2 connection is closed with GOAWAY", "[http][http2][server][timeouts]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, {.header_read_timeout = 1s}};

    const auto start = std::chrono::steady_clock::now();
    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const auto id = c.request("GET", "/");
                                            REQUIRE(co_await c.wait_for(id));

                                            // then nothing, until the server ends it.
                                            (void)co_await c.pump([&] { return c.closed; });
                                            CHECK(c.goaway);
                                        }));
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}

TEST_CASE("closing a server ends its HTTP/2 connections gracefully", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&server](h2_client& c) -> task<>
                                        {
                                            // a request in progress when the server's closed still gets its response.
                                            // NOTE: the server's SETTINGS say it's serving the connection (closing the
                                            // server before it
                                            //       had accepted it would leave it unanswered in the listener's queue).
                                            //       It reads the request before the client's acknowledgement of its
                                            //       PING, so its final GOAWAY includes it.
                                            const auto slow = c.request("GET", "/slow");
                                            REQUIRE(co_await c.pump([&] { return c.s.has_peer_settings(); }));

                                            server.server.close();
                                            REQUIRE(co_await c.wait_for(slow));
                                            CHECK(c.responses[slow].body == "slow");

                                            (void)co_await c.pump([&] { return c.closed; });
                                            CHECK(c.goaway);
                                        }));
}

TEST_CASE("HTTP/1.1 is still served on the same port", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    const auto test = [](net::io::scheduler* sched, std::string port) -> task<std::string>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        REQUIRE(conn.has_value());
        co_await net::io::write_all(*conn, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"sv);
        auto response = co_await net::io::read_all(*conn);
        co_return response.value_or("");
    };

    const auto response = sched.run_to_completion(test(&sched, server.port()));
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(response.ends_with("hello over HTTP/1"));
}

TEST_CASE("without http2, a client's HTTP/2 preface isn't understood", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, {.http2 = false}};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const auto id = c.request("GET", "/");
                                            (void)co_await c.wait_for(id);
                                            CHECK_FALSE(c.responses[id].done);
                                            CHECK(c.closed);
                                        }));
}

TEST_CASE("request bodies HTTP/2 handlers don't read give the connection's window back", "[http][http2][server][flow]")
{
    net::io::scheduler sched{make_pool()};
    gate               g{&sched};

    auto r = make_router(&sched);
    r.POST("/ignore",
           [&g](const http::server_request&, http::response_writer& resp) -> task<void>
           {
               co_await g.pass();
               co_await resp.send(http::status::OK, 0);
           });

    // with 10 streams, the connection's window is 1 MiB.
    test_server server{&sched, std::move(r), {.http2_max_concurrent_streams = 10}};

    sched.run_to_completion(with_client(
        &sched,
        server.port(),
        [&g](h2_client& c) -> task<>
        {
            // 30 bodies of 60 KB (1.8 MB), each received whole before its handler returns without reading it.
            const std::string body(60'000, 'x');
            for (int i = 0; i < 30; ++i)
            {
                const auto id = c.request("POST", "/ignore", body);
                REQUIRE(co_await c.sync());
                g.open();
                REQUIRE(co_await c.wait_for(id));
                REQUIRE(c.responses[id].status == "200");
            }

            // then an upload, which needs the connection's window.
            const std::string upload(300'000, 'u');
            const auto        id = c.request("POST", "/echo", upload);
            REQUIRE(co_await c.wait_for(id));
            CAPTURE(c.responses[id].reset.message());
            CHECK(c.responses[id].body == upload);
        },
        5s));
}

TEST_CASE("an HTTP/2 handler's reads give the window back at once, though it stops reading",
          "[http][http2][server][flow]")
{
    net::io::scheduler sched{make_pool()};
    gate               g{&sched};

    constexpr std::size_t first = 40'000;

    auto r = make_router(&sched);
    r.POST("/partial",
           [&g](http::server_request& req, http::response_writer& resp) -> task<void>
           {
               // once the stream's window of the body has arrived, part of it is read (more than half the window, so
               // the peer's owed a WINDOW_UPDATE), without the reads finding the buffer empty.
               co_await g.pass();
               std::vector<std::byte> buffer(4'096);
               for (std::size_t read = 0; read < first;)
               {
                   const auto res =
                       co_await req.body->read(std::span{buffer}.first(std::min(buffer.size(), first - read)));
                   if (res.err) throw std::system_error{res.err};
                   read += res.count;
               }

               // then nothing more, until the client has sent the rest (which needs that WINDOW_UPDATE).
               co_await g.pass();
               auto rest = co_await net::io::read_all(*req.body);
               if (!rest.has_value()) throw std::system_error{rest.error()};

               const auto size = std::to_string(first + rest->size());
               auto*      body = co_await resp.send(http::status::OK, size.size());
               co_await net::io::write_all(*body, size);
           });

    // NOTE: the connection's timer wakes the writer too (at the idle timeout it set before the request arrived, which
    //       is header_read_timeout): not before the client's reads time out.
    test_server server{&sched, std::move(r), {.header_read_timeout = 60s}};

    sched.run_to_completion(with_client(
        &sched,
        server.port(),
        [&g](h2_client& c) -> task<>
        {
            // a stream's window (65535) is sent at once; the rest fits in what the reads give back.
            const std::string body(90'000, 'x');
            const auto        id = c.request("POST", "/partial", body);
            REQUIRE(co_await c.sync());
            g.open();

            REQUIRE(co_await c.pump([&] { return c.pending_body.empty(); }));
            g.open();

            REQUIRE(co_await c.wait_for(id));
            CHECK(c.responses[id].body == "90000");
        },
        5s));
}

TEST_CASE("an HTTP/2 handler that throws mid-body has its stream reset, rather than its response ended",
          "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const auto with_length    = c.request("GET", "/throw-mid-body");
                                            const auto without_length = c.request("GET", "/throw-mid-stream");
                                            REQUIRE(co_await c.pump([&] { return c.finished == 2; }));

                                            // the server's reset, not one for a Content-Length the client found short.
                                            CHECK(c.responses[with_length].reset == http2::error_code::internal_error);
                                            CHECK(c.responses[without_length].reset
                                                  == http2::error_code::internal_error);
                                        }));
}

TEST_CASE("an HTTP/2 handler that throws after an interim response gets a 500, without its headers",
          "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const auto id = c.request("GET", "/throw-after-interim");
                                            REQUIRE(co_await c.wait_for(id));

                                            const auto& r = c.responses[id];
                                            CAPTURE(r.reset.message());
                                            CHECK(r.status == "500");
                                            CHECK(header(r, "x-meant-for-200") == "(none)");
                                            CHECK(header(r, "date") != "(none)");
                                        }));
}

TEST_CASE("WINDOW_UPDATEs on the connection don't put off an HTTP/2 stream's body_read_timeout",
          "[http][http2][server][timeouts]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, {.body_read_timeout = 1s}};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            // a request whose body never comes, while the connection's window grows
                                            // every 0.3 s (which wakes the stream's reader).
                                            const auto stalled = c.request("POST", "/echo", "", false);
                                            const auto start   = std::chrono::steady_clock::now();
                                            while (!c.responses[stalled].done && !c.closed
                                                   && std::chrono::steady_clock::now() - start < 4s)
                                            {
                                                (void)co_await c.wait_for(stalled, 300ms);
                                                REQUIRE(co_await c.window_update(0, 1));
                                            }

                                            CHECK(c.responses[stalled].reset == http2::error_code::cancel);
                                            CHECK(std::chrono::steady_clock::now() - start < 3s);
                                        }));
}

TEST_CASE("an HTTP/2 client resetting its streams can't run more handlers than it may open streams",
          "[http][http2][server][limits]")
{
    net::io::scheduler sched{make_pool()};
    gate               g{&sched};

    auto r = make_router(&sched);
    r.GET("/hold",
          [&g](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              co_await g.pass();
              co_await resp.send(http::status::OK, 0);
          });

    test_server server{&sched, std::move(r), {.http2_max_concurrent_streams = 2}};

    SECTION("past the limit, a stream's handler waits for another to finish")
    {
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&g](h2_client& c) -> task<>
                                            {
                                                // two handlers held; one's stream reset (which frees the stream, but
                                                // not the handler); then another.
                                                const auto first  = c.request("GET", "/hold");
                                                const auto second = c.request("GET", "/hold");
                                                c.s.reset(first, http2::error_code::cancel);
                                                const auto third = c.request("GET", "/");

                                                // it waits, though it's quick, until a held one finishes.
                                                CHECK_FALSE(co_await c.wait_for(third, 300ms));

                                                g.open();
                                                REQUIRE(co_await c.wait_for(third));
                                                CHECK(c.responses[third].body == "hello over HTTP/2");

                                                g.open();
                                                REQUIRE(co_await c.wait_for(second));
                                                CHECK(c.responses[second].status == "200");
                                            }));
    }

    SECTION("too many waiting, the connection's ended")
    {
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [](h2_client& c) -> task<>
                                            {
                                                // streams opened and reset at once, whose handlers never finish.
                                                for (int i = 0; i < 20; ++i)
                                                {
                                                    const auto id = c.request("GET", "/hold");
                                                    c.s.reset(id, http2::error_code::cancel);
                                                }

                                                // NOTE: the connection's closed once its handlers have finished,
                                                //       which these never do.
                                                REQUIRE(co_await c.pump([&] { return c.goaway; }, 3s));
                                                CHECK(c.goaway_error == http2::error_code::enhance_your_calm);
                                            }));
    }
}

TEST_CASE("an HTTP/2 response's field with a line break throws, as over HTTP/1.1", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const auto id = c.request("GET", "/bad-header");
                                            REQUIRE(co_await c.wait_for(id));

                                            const auto& r = c.responses[id];
                                            CAPTURE(r.reset.message());
                                            CHECK(r.status == "200");
                                            CHECK(r.body == "threw");
                                            CHECK(header(r, "x-bad") == "(none)");
                                        }));
}
