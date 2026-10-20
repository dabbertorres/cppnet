#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
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
#include "http/hpack.hpp"
#include "http/http.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
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
#include "socket.hpp"
#include "tcp.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::gate;
using test::h2_client;
using test::header;
using test::stop_probe;

namespace hpack = net::http::hpack;

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

    // NOTE: one connection at a time, so a second client waits for the first's slot.
    test_server server{
        &sched,
        std::move(r),
        {.max_connections = 1, .http2_max_concurrent_streams = 2}
    };

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

    SECTION("too many waiting, the connection's ended at once, though its handlers hold its slot")
    {
        const auto port = server.port();
        sched.run_to_completion(with_client(&sched,
                                            port,
                                            [&g, &sched, port](h2_client& c) -> task<>
                                            {
                                                // streams opened and reset at once, whose handlers never finish: the
                                                // first two run, the rest wait, until the ninth is one too many.
                                                for (int i = 0; i < 20; ++i)
                                                {
                                                    const auto id = c.request("GET", "/hold");
                                                    c.s.reset(id, http2::error_code::cancel);
                                                }

                                                // the connection's closed, though its handlers are held (RFC 9113,
                                                // section 5.4.1).
                                                const bool closed = co_await c.pump([&] { return c.closed; }, 3s);
                                                REQUIRE(closed);
                                                CHECK(c.goaway);
                                                CHECK(c.goaway_error == http2::error_code::enhance_your_calm);

                                                // the streams after the last started (3) never will be, so they may be
                                                // retried.
                                                CHECK(c.goaway_last == 3);

                                                // the handlers still hold its slot (max_connections), until they
                                                // finish.
                                                co_await with_client(
                                                    &sched,
                                                    port,
                                                    [&g](h2_client& next) -> task<>
                                                    {
                                                        const auto id    = next.request("GET", "/");
                                                        const bool early = co_await next.wait_for(id, 300ms);
                                                        CHECK_FALSE(early);

                                                        g.open();
                                                        g.open();
                                                        const bool answered = co_await next.wait_for(id);
                                                        REQUIRE(answered);
                                                        CHECK(next.responses[id].body == "hello over HTTP/2");
                                                    });
                                            }));
    }
}

TEST_CASE("an HTTP/2 client that closes its side gets the server's FIN at once while a handler is held",
          "[http][http2][server]")
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

    test_server server{&sched, std::move(r)};

    const auto test = [](net::io::scheduler* sched, std::string port) -> task<>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        REQUIRE(conn.has_value());

        h2_client  c{&*conn, &*conn, &*conn};
        const auto id     = c.request("GET", "/hold");
        const bool synced = co_await c.sync(); // the server has the request (and nothing's left to send)
        REQUIRE(synced);

        conn->shutdown_write();

        const auto start  = std::chrono::steady_clock::now();
        const bool closed = co_await c.pump([&] { return c.closed; }, 3s);
        CHECK(closed);
        CHECK(std::chrono::steady_clock::now() - start < 1s);
        CHECK_FALSE(c.responses[id].done);
    };

    sched.run_to_completion(test(&sched, server.port()));
}

TEST_CASE("an HTTP/2 connection that ends reads what the client still sends, rather than cutting off the response",
          "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};

    // more than TCP's buffers hold while the client doesn't read.
    constexpr std::size_t      size = std::size_t{8} << 20;
    std::atomic<std::uint64_t> written{0};

    auto r = make_router(&sched);
    r.GET("/huge",
          [&written, &sched](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              // at once (into the session: the windows don't hold it back), so the handler returns.
              const std::string body(size, 'h');
              auto*             out = co_await resp.send(http::status::OK, body.size());
              co_await net::io::write_all(*out, body);

              written.store(1);
              sched.notify_all(written);
          });

    test_server server{&sched, std::move(r)};

    const auto test = [](net::io::scheduler* sched, std::string port, std::atomic<std::uint64_t>* written) -> task<>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        REQUIRE(conn.has_value());

        // windows that don't hold the response back: only TCP's do.
        http2::session_config config;
        config.local.initial_window_size = 0x7fff'ffff;
        config.connection_window         = 0x7fff'ffff;
        h2_client c{&*conn, &*conn, &*conn, http2::role::client, config};

        // the request, and GOAWAY: the server ends the connection once the handler's done.
        const auto id = c.request("GET", "/huge");
        c.s.goaway(0, http2::error_code::no_error);

        std::vector<std::byte> output;
        c.s.take_output(output);
        const auto sent = co_await net::io::write_all(*conn, output);
        REQUIRE_FALSE(sent.err);

        // the handler's returned, with most of the response still to be sent, and (by now) the connection's ended.
        for (auto seen = written->load(); seen == 0; seen = written->load())
            (void)co_await sched->wait_on(*written, seen);
        (void)co_await sched->sleep_until(net::io::clock::now() + 100ms);

        // the client sends something more (as it sends WINDOW_UPDATEs as it reads): the server mustn't reset the
        // connection for it (e.g. for input after shutting down its reading side, on macOS; or closing with it unread),
        // which would discard what it's still sending.
        constexpr std::array<std::byte, 8> opaque{};
        c.s.ping(opaque);

        const bool done = co_await c.wait_for(id);
        CHECK(done);

        const auto& resp = c.responses[id];
        CAPTURE(resp.reset.message());
        CHECK(resp.status == "200");
        CHECK(resp.body.size() == size);
    };

    sched.run_to_completion(test(&sched, server.port(), &written));
}

TEST_CASE("HTTP/2's header list limit doesn't wrap with max_header_bytes near SIZE_MAX",
          "[http][http2][server][limits]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, {.max_header_bytes = SIZE_MAX}};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            // more than 32 bytes a field for HTTP/1.1's most fields (3,200), which
                                            // is all a limit that wrapped would allow.
                                            const std::string                 value(5'000, 'v');
                                            const std::array<hpack::field, 1> fields{
                                                hpack::field{.name = "x-large", .value = value}
                                            };

                                            const auto id   = c.request("GET", "/", {}, true, fields);
                                            const bool done = co_await c.wait_for(id);
                                            REQUIRE(done);

                                            const auto& resp = c.responses[id];
                                            CAPTURE(resp.reset.message());
                                            CHECK(resp.status == "200");
                                            CHECK(resp.body == "hello over HTTP/2");
                                        }));
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

namespace
{

// probe_router is make_router's, with handlers that wait on their request's token:
// - /wait waits, then answers 200 "waited" if it wasn't stopped, or 503 "aborted" if the server was aborted, or
//   "stopped" if it was stopped otherwise;
// - /wait-silent waits, then returns without answering;
// - /partial sends a head (with a Content-Length of 100) and some of its body, then waits;
// - /nap waits a little (or until stopped), then answers "rested" or "stopped".
http::router probe_router(net::io::scheduler* sched, stop_probe* probe)
{
    auto r = make_router(sched);
    r.GET("/wait",
          [sched, probe](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              const bool stopped = co_await probe->wait(sched, req.stop);
              auto       msg     = "waited"sv;
              if (stopped) msg = req.aborted.stop_requested() ? "aborted"sv : "stopped"sv;
              auto* body =
                  co_await resp.send(stopped ? http::status::SERVICE_UNAVAILABLE : http::status::OK, msg.size());
              co_await net::io::write_all(*body, msg);
          });
    r.GET("/wait-silent",
          [sched, probe](const http::server_request& req, http::response_writer&) -> task<void>
          { (void)co_await probe->wait(sched, req.stop); });
    r.GET("/partial",
          [sched, probe](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              auto* body = co_await resp.send(http::status::OK, 100);
              co_await net::io::write_all(*body, "partial"sv);
              (void)co_await probe->wait(sched, req.stop);
          });
    r.GET("/nap",
          [sched, probe](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              stop_probe::bump(sched, probe->started);
              const auto err = co_await sched->sleep_until(net::io::clock::now() + 300ms, req.stop);
              const auto msg = err ? "stopped"sv : "rested"sv;
              stop_probe::bump(sched, probe->woke);

              auto* body = co_await resp.send(http::status::OK, msg.size());
              co_await net::io::write_all(*body, msg);
          });
    return r;
}

}

TEST_CASE("an HTTP/2 handler's token is stopped when the client resets its stream, and only its",
          "[http][http2][server][cancel]")
{
    // NOTE: before the scheduler, as the handlers use it.
    stop_probe probe;

    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, probe_router(&sched, &probe)};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&](h2_client& c) -> task<>
                                        {
                                            const auto reset = c.request("GET", "/wait");
                                            const auto other = c.request("GET", "/slow");
                                            REQUIRE(co_await c.sync());
                                            REQUIRE(co_await stop_probe::reach(&sched, probe.started, 1));

                                            const auto start = std::chrono::steady_clock::now();
                                            c.s.reset(reset, http2::error_code::cancel);

                                            // the other stream goes on.
                                            REQUIRE(co_await c.wait_for(other));
                                            CHECK(c.responses[other].body == "slow");

                                            REQUIRE(co_await stop_probe::reach(&sched, probe.woke, 1));
                                            CHECK(probe.canceled.load() == 1);
                                            CHECK(std::chrono::steady_clock::now() - start < 5s);

                                            // and the connection.
                                            const auto next = c.request("GET", "/");
                                            REQUIRE(co_await c.wait_for(next));
                                            CHECK(c.responses[next].status == "200");
                                        }));
}

TEST_CASE("an HTTP/2 handler's token is stopped when its stream times out", "[http][http2][server][cancel]")
{
    // NOTE: before the scheduler, as the handlers use them.
    stop_probe                 probe;
    std::atomic<std::uint64_t> read_done{0};

    net::io::scheduler sched{make_pool()};

    // reads its body in a task of its own (which times out, as the body never comes), while it waits on its token.
    auto r = probe_router(&sched, &probe);
    r.POST("/read-aside",
           [&](http::server_request& req, http::response_writer&) -> task<void>
           {
               REQUIRE(sched.spawn(
                   [](net::io::scheduler* sched, net::io::reader* body, std::atomic<std::uint64_t>* done) -> task<>
                   {
                       (void)co_await net::io::read_all(*body);
                       stop_probe::bump(sched, *done);
                   }(&sched, req.body.get(), &read_done)));

               (void)co_await probe.wait(&sched, req.stop);

               // the body's reader is the stream's, which goes once the handler returns.
               (void)co_await stop_probe::reach(&sched, read_done, 1);
           });

    test_server server{&sched, std::move(r), {.body_read_timeout = 1s}};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&](h2_client& c) -> task<>
                                        {
                                            const auto stalled = c.request("POST", "/read-aside", "", false);
                                            REQUIRE(co_await c.wait_for(stalled));
                                            CHECK(c.responses[stalled].reset == http2::error_code::cancel);

                                            REQUIRE(co_await stop_probe::reach(&sched, probe.woke, 1));
                                            CHECK(probe.canceled.load() == 1);
                                        }));
}

TEST_CASE("an HTTP/2 handler's token is stopped when its connection ends", "[http][http2][server][cancel]")
{
    // NOTE: before the scheduler, as the handlers use it.
    stop_probe probe;

    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, probe_router(&sched, &probe)};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&](h2_client& c) -> task<>
                                        {
                                            (void)c.request("GET", "/wait");
                                            (void)c.request("GET", "/wait");
                                            REQUIRE(co_await c.sync());
                                            REQUIRE(co_await stop_probe::reach(&sched, probe.started, 2));

                                            // the client closes its side.
                                            c.sock->shutdown_write();

                                            REQUIRE(co_await stop_probe::reach(&sched, probe.woke, 2));
                                            CHECK(probe.canceled.load() == 2);
                                        }));
}

TEST_CASE("HTTP/2 handlers that heed their tokens free their connection's slot once it's ended",
          "[http][http2][server][limits][cancel]")
{
    net::io::scheduler sched{make_pool()};
    gate               g{&sched};

    auto r = make_router(&sched);
    r.GET("/hold",
          [&g](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              if (co_await g.pass(req.stop)) co_return;
              co_await resp.send(http::status::OK, 0);
          });

    // NOTE: one connection at a time, so a second client waits for the first's slot.
    test_server server{
        &sched,
        std::move(r),
        {.max_connections = 1, .http2_max_concurrent_streams = 2}
    };

    const auto port = server.port();
    sched.run_to_completion(with_client(&sched,
                                        port,
                                        [&sched, port](h2_client& c) -> task<>
                                        {
                                            // as the group-5 test: streams opened and reset at once, until the
                                            // connection's ended with ENHANCE_YOUR_CALM.
                                            for (int i = 0; i < 20; ++i)
                                            {
                                                const auto id = c.request("GET", "/hold");
                                                c.s.reset(id, http2::error_code::cancel);
                                            }

                                            const bool closed = co_await c.pump([&] { return c.closed; }, 3s);
                                            REQUIRE(closed);
                                            CHECK(c.goaway_error == http2::error_code::enhance_your_calm);

                                            // the handlers return (their tokens stopped), so the next client's
                                            // served without the gate opening.
                                            co_await with_client(
                                                &sched,
                                                port,
                                                [](h2_client& next) -> task<>
                                                {
                                                    const auto id       = next.request("GET", "/");
                                                    const bool answered = co_await next.wait_for(id, 3s);
                                                    REQUIRE(answered);
                                                    CHECK(next.responses[id].body == "hello over HTTP/2");
                                                });
                                        }));
}

TEST_CASE("the client's GOAWAY doesn't stop the HTTP/2 handlers running", "[http][http2][server][cancel]")
{
    // NOTE: before the scheduler, as the handlers use it.
    stop_probe probe;

    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, probe_router(&sched, &probe)};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&](h2_client& c) -> task<>
                                        {
                                            const auto id = c.request("GET", "/nap");
                                            REQUIRE(co_await c.sync());
                                            REQUIRE(co_await stop_probe::reach(&sched, probe.started, 1));

                                            c.s.goaway(0, http2::error_code::no_error);
                                            REQUIRE(co_await c.wait_for(id));
                                            CHECK(c.responses[id].body == "rested");
                                        }));
}

TEST_CASE("closing the server doesn't stop HTTP/2 handlers' tokens; aborting it does", "[http][http2][server][cancel]")
{
    // NOTE: before the scheduler, as the handlers use it.
    stop_probe probe;

    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, probe_router(&sched, &probe)};

    // starts a request for path, and waits for its handler to start.
    const auto start = [&](h2_client& c, std::string_view path) -> task<http2::stream_id>
    {
        const auto id = c.request("GET", path);
        REQUIRE(co_await c.sync());
        REQUIRE(co_await stop_probe::reach(&sched, probe.started, 1));
        co_return id;
    };

    SECTION("closed: the handler finishes")
    {
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&](h2_client& c) -> task<>
                                            {
                                                const auto id = co_await start(c, "/nap");
                                                server.server.close();

                                                REQUIRE(co_await c.wait_for(id));
                                                CHECK(c.responses[id].body == "rested");
                                            }));
    }

    SECTION("aborted: a handler may still answer")
    {
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&](h2_client& c) -> task<>
                                            {
                                                const auto id = co_await start(c, "/wait");
                                                server.server.abort();

                                                REQUIRE(co_await c.wait_for(id));
                                                CHECK(c.responses[id].status == "503");
                                                CHECK(c.responses[id].body == "aborted");
                                                CHECK(probe.canceled.load() == 1);
                                            }));
    }

    SECTION("aborted: an incomplete response is reset with CANCEL")
    {
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&](h2_client& c) -> task<>
                                            {
                                                const auto id = co_await start(c, "/partial");
                                                server.server.abort();

                                                REQUIRE(co_await c.wait_for(id));
                                                CHECK(c.responses[id].status == "200");
                                                CHECK(c.responses[id].body == "partial");
                                                CHECK(c.responses[id].reset == http2::error_code::cancel);
                                            }));
    }

    SECTION("aborted: a handler that doesn't answer gets no answer from the server")
    {
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&](h2_client& c) -> task<>
                                            {
                                                const auto id = co_await start(c, "/wait-silent");
                                                server.server.abort();

                                                REQUIRE(co_await c.wait_for(id));
                                                CHECK(c.responses[id].status.empty());
                                                CHECK(c.responses[id].reset == http2::error_code::cancel);
                                            }));
    }

    SECTION("aborted: a stream started after is stopped from the start")
    {
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&](h2_client& c) -> task<>
                                            {
                                                const auto first = co_await start(c, "/nap");
                                                server.server.abort();

                                                // (the drain's first GOAWAY still lets the client open streams.)
                                                const auto later = c.request("GET", "/wait");
                                                REQUIRE(co_await c.wait_for(later));
                                                CHECK(c.responses[later].status == "503");
                                                CHECK(c.responses[later].body == "aborted");

                                                REQUIRE(co_await c.wait_for(first));
                                                CHECK(c.responses[first].body == "stopped");
                                            }));
    }
}

TEST_CASE("an HTTP/2 interim response is sent at once, as a HEADERS frame before the final one",
          "[http][http2][server][interim]")
{
    net::io::scheduler sched{make_pool()};
    gate               g{&sched};

    auto r = make_router(&sched);

    // sends Early Hints, then waits for the client to have read them, before the final response (which keeps their
    // fields).
    r.GET("/hint",
          [&g](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          {
              resp.headers().set("Link", "</style.css>; rel=preload");
              co_await resp.send(http::status::EARLY_HINTS);
              resp.headers().add("Link", "</app.js>; rel=preload");
              co_await resp.send(http::status::EARLY_HINTS);
              co_await g.pass();

              auto* body = co_await resp.send(http::status::OK, 4);
              co_await net::io::write_all(*body, "done"sv);
          });

    test_server server{&sched, std::move(r)};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&g](h2_client& c) -> task<>
                                        {
                                            const auto id = c.request("GET", "/hint");
                                            const bool hinted =
                                                co_await c.pump([&] { return c.responses[id].interims.size() == 2; });
                                            REQUIRE(hinted);

                                            const auto& r = c.responses[id];
                                            CHECK(r.interims[0].status == "103");
                                            CHECK(header(r.interims[0], "link") == "</style.css>; rel=preload");
                                            CHECK(header(r.interims[0], "content-length") == "(none)");
                                            CHECK(r.interims[1].status == "103");
                                            CHECK(r.interims[1].headers.size() == r.interims[0].headers.size() + 1);

                                            // not the end of the stream, nor the final response.
                                            CHECK_FALSE(r.done);
                                            CHECK(r.status.empty());

                                            g.open();
                                            const bool finished = co_await c.wait_for(id);
                                            REQUIRE(finished);
                                            CAPTURE(r.reset.message());
                                            CHECK(r.status == "200");
                                            CHECK(header(r, "link") == "</style.css>; rel=preload");
                                            CHECK(r.body == "done");
                                            CHECK(r.interims.size() == 2);
                                        }));
}

TEST_CASE("an HTTP/2 handler can't send 101 Switching Protocols", "[http][http2][server][interim]")
{
    net::io::scheduler sched{make_pool()};

    auto r = make_router(&sched);
    r.GET("/switch",
          [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          {
              std::string outcome = "sent";
              try
              {
                  co_await resp.send(http::status::SWITCHING_PROTOCOLS);
              }
              catch (const std::system_error& e)
              {
                  outcome = e.code() == std::errc::not_supported ? "refused" : e.what();
              }

              auto* body = co_await resp.send(http::status::OK, outcome.size());
              co_await net::io::write_all(*body, outcome);
          });

    test_server server{&sched, std::move(r)};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const auto id       = c.request("GET", "/switch");
                                            const bool finished = co_await c.wait_for(id);
                                            REQUIRE(finished);

                                            const auto& r = c.responses[id];
                                            CHECK(r.interims.empty());
                                            CHECK(r.status == "200");
                                            CHECK(r.body == "refused");
                                        }));
}

TEST_CASE("an HTTP/2 request that expects 100 Continue gets one when its handler reads the body",
          "[http][http2][server][expect]")
{
    // NOTE: before the scheduler, which waits for the tasks that use it.
    std::atomic<std::uint64_t> started{0};

    net::io::scheduler sched{make_pool()};
    gate               g{&sched};

    auto r = make_router(&sched);

    // waits to be let through before reading the body.
    r.POST("/wait",
           [&](http::server_request& req, http::response_writer& resp) -> task<void>
           {
               stop_probe::bump(&sched, started);
               co_await g.pass();
               co_await echo(req, resp);
           });

    // answers without reading the body.
    r.POST("/refuse",
           [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
           { co_await resp.send(http::status::PAYLOAD_TOO_LARGE, 0); });

    // sends Early Hints, then reads the body.
    r.POST("/hint",
           [](http::server_request& req, http::response_writer& resp) -> task<void>
           {
               resp.headers().set("Link", "</style.css>; rel=preload");
               co_await resp.send(http::status::EARLY_HINTS);
               co_await echo(req, resp);
           });

    test_server server{&sched, std::move(r)};

    const std::array expect{
        hpack::field{.name = "expect", .value = "100-continue"}
    };

    SECTION("only once the handler reads, before the client sends the body")
    {
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&](h2_client& c) -> task<>
                                            {
                                                // (the request goes with the PING.)
                                                const auto id   = c.request("POST", "/wait", {}, false, expect);
                                                const bool sent = co_await c.sync();
                                                REQUIRE(sent);
                                                const bool in = co_await stop_probe::reach(&sched, started, 1);
                                                REQUIRE(in);

                                                // by the PING's answer, whatever the server sent before it is here.
                                                const bool synced = co_await c.sync();
                                                REQUIRE(synced);
                                                CHECK(c.responses[id].interims.empty());

                                                g.open();
                                                const bool asked =
                                                    co_await c.pump([&] { return !c.responses[id].interims.empty(); });
                                                REQUIRE(asked);

                                                // just the status.
                                                const auto& cont = c.responses[id].interims.front();
                                                CHECK(cont.status == "100");
                                                CHECK(cont.headers.empty());

                                                c.send(id, "hello", true);
                                                const bool finished = co_await c.wait_for(id);
                                                REQUIRE(finished);

                                                const auto& resp = c.responses[id];
                                                CAPTURE(resp.reset.message());
                                                CHECK(resp.status == "200");
                                                CHECK(resp.body == "hello");
                                                CHECK(resp.interims.size() == 1);
                                            }));
    }

    SECTION("one after the handler's Early Hints, which don't stop it")
    {
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&](h2_client& c) -> task<>
                                            {
                                                const auto id    = c.request("POST", "/hint", {}, false, expect);
                                                const bool asked = co_await c.pump(
                                                    [&] { return c.responses[id].interims.size() == 2; });
                                                REQUIRE(asked);

                                                // the 103, then the 100 (just the status), before the body's sent.
                                                const auto& resp = c.responses[id];
                                                CHECK(resp.interims[0].status == "103");
                                                CHECK(header(resp.interims[0], "link") == "</style.css>; rel=preload");
                                                CHECK(resp.interims[1].status == "100");
                                                CHECK(resp.interims[1].headers.empty());
                                                CHECK_FALSE(resp.done);
                                                CHECK(resp.status.empty());

                                                c.send(id, "hello", true);
                                                const bool finished = co_await c.wait_for(id);
                                                REQUIRE(finished);

                                                CAPTURE(resp.reset.message());
                                                CHECK(resp.status == "200");
                                                CHECK(header(resp, "link") == "</style.css>; rel=preload");
                                                CHECK(resp.body == "hello");
                                                CHECK(resp.interims.size() == 2);
                                            }));
    }

    SECTION("none for a handler that answers without reading")
    {
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&](h2_client& c) -> task<>
                                            {
                                                const auto id       = c.request("POST", "/refuse", {}, false, expect);
                                                const bool finished = co_await c.wait_for(id);
                                                REQUIRE(finished);

                                                const auto& resp = c.responses[id];
                                                CHECK(resp.status == "413");
                                                CHECK(resp.interims.empty());
                                            }));
    }

    SECTION("417 for another expectation, without running the handler")
    {
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&](h2_client& c) -> task<>
                                            {
                                                const std::array other{
                                                    hpack::field{.name = "expect", .value = "something"}
                                                };
                                                const auto id       = c.request("POST", "/wait", "hello", true, other);
                                                const bool finished = co_await c.wait_for(id);
                                                REQUIRE(finished);

                                                const auto& resp = c.responses[id];
                                                CHECK(resp.status == "417");
                                                CHECK(resp.interims.empty());
                                                CHECK(started.load() == 0);
                                            }));
    }
}
