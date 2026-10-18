#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
    return r;
}

struct test_server
{
    explicit test_server(net::io::scheduler* sched, const http::server_config& cfg = {})
        : server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), make_router(sched), cfg}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    [[nodiscard]] std::string port() const { return std::to_string(server.local_port()); }

    http::server server;
};

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// with_client connects to port, and runs test with an h2_client over the connection.
template<typename Test>
task<> with_client(net::io::scheduler* sched, std::string port, Test test)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
    REQUIRE(conn.has_value());

    h2_client client{&*conn, &*conn};
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
