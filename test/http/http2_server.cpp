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

// seen is what an h2_client saw on its connection, copied out for the test to check once the scheduler's done.
struct seen
{
    std::map<http2::stream_id, h2_client::response>                 responses;
    std::map<http2::stream_id, std::vector<std::vector<std::byte>>> blocks;
    bool                                                            goaway = false;
    std::error_code                                                 goaway_error;
    http2::stream_id                                                goaway_last = 0;
    bool                                                            closed      = false;
};

// with_client connects to port, and runs test with an h2_client of config's over the connection (whose reads fail after
// read_timeout without progress, if it's given: for a test whose server could stop sending), keeping what the client
// saw in out (if given). It returns what went wrong, if anything: test returns what went wrong with its own steps, to
// which a submission the client's session refused is added.
template<typename Test>
task<std::string> with_client(net::io::scheduler*                     sched,
                              std::string                             port,
                              seen*                                   out,
                              Test                                    test,
                              std::optional<net::io::clock::duration> read_timeout = std::nullopt,
                              http2::session_config                   config       = {})
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
    if (!conn.has_value()) co_return "not connected: " + conn.error().message();
    conn->set_read_timeout(read_timeout);

    h2_client   client{&*conn, &*conn, &*conn, http2::role::client, config};
    std::string failed = co_await test(client);
    if (!client.failure.empty()) failed += (failed.empty() ? "" : "; ") + client.failure;

    if (out != nullptr)
    {
        out->responses    = client.responses;
        out->blocks       = client.blocks;
        out->goaway       = client.goaway;
        out->goaway_error = client.goaway_error;
        out->goaway_last  = client.goaway_last;
        out->closed       = client.closed;
    }
    co_return failed;
}

}

TEST_CASE("HTTP/2 with prior knowledge: a request and its response", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    seen             out;
    http2::stream_id id = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                id              = c.request("GET", "/");
                                                                const bool done = co_await c.wait_for(id);
                                                                co_return done ? "" : "no response";
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    const auto& r = out.responses[id];
    CHECK(r.status == "200");
    CHECK(r.body == "hello over HTTP/2");
    CHECK(header(r, "content-type") == "text/plain");
    CHECK(header(r, "content-length") == "17");
    CHECK(header(r, "date") != "(none)");
}

TEST_CASE("HTTP/2 bodies larger than the windows go both ways", "[http][http2][server][flow]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    std::string body(300'000, 'x');
    for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>('a' + (i % 26));

    seen             out;
    http2::stream_id id = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                id              = c.request("POST", "/echo", body);
                                                                const bool done = co_await c.wait_for(id);
                                                                co_return done ? "" : "no response";
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    const auto& r = out.responses[id];
    CAPTURE(r.reset.message());
    CHECK(r.status == "200");
    CHECK(r.body.size() == body.size());
    CHECK(r.body == body);
}

TEST_CASE("HTTP/2 streams are served at once: a slow one doesn't hold up the others", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    gate               g{&sched};

    // /held is the slow one: held until the test lets it go, once the others have all been answered.
    auto r = make_router(&sched);
    r.GET("/held",
          [&g](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              co_await g.pass();
              auto* body = co_await resp.send(http::status::OK, 4);
              co_await net::io::write_all(*body, "slow"sv);
          });
    test_server server{&sched, std::move(r)};

    seen                          out;
    http2::stream_id              slow = 0;
    std::vector<http2::stream_id> fast;
    bool                          slow_done_first = false; // the slow one was done before it was let go

    const auto failed = sched.run_to_completion(
        with_client(&sched,
                    server.port(),
                    &out,
                    [&](h2_client& c) -> task<std::string>
                    {
                        slow = c.request("GET", "/held");
                        fast.reserve(10);
                        for (int i = 0; i < 10; ++i) fast.push_back(c.request("GET", "/"));

                        // the others are all answered while the slow one's held.
                        const bool answered = co_await c.pump(
                            [&] { return std::ranges::all_of(fast, [&](auto id) { return c.responses[id].done; }); });
                        if (!answered) co_return "the others weren't answered";
                        slow_done_first = c.responses[slow].done;

                        g.open();
                        const bool done = co_await c.wait_for(slow);
                        co_return done ? "" : "the slow one wasn't answered";
                    }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    // the slow one finished last.
    CHECK_FALSE(slow_done_first);
    CHECK(out.responses[slow].body == "slow");
    CHECK(out.responses[slow].order == 11);
    for (const auto id : fast) CHECK(out.responses[id].body == "hello over HTTP/2");
}

TEST_CASE("HTTP/2 handlers that throw, HEAD, and unknown methods", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    seen             out;
    http2::stream_id thrown  = 0;
    http2::stream_id head    = 0;
    http2::stream_id unknown = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                thrown  = c.request("GET", "/throw");
                                                                head    = c.request("HEAD", "/");
                                                                unknown = c.request("BREW", "/");
                                                                const bool done =
                                                                    co_await c.pump([&] { return c.finished == 3; });
                                                                co_return done ? "" : "not all of them ended";
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(out.responses[thrown].status == "500");
    CHECK(out.responses[head].status == "200");
    CHECK(header(out.responses[head], "content-length") == "17");
    CHECK(out.responses[head].body.empty());
    CHECK(out.responses[unknown].status == "501");
    CHECK(header(out.responses[unknown], "date") != "(none)");
}

TEST_CASE("an HTTP/2 stream whose body stops arriving is reset alone", "[http][http2][server][timeouts]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, {.body_read_timeout = 1s}};

    seen             out;
    http2::stream_id stalled = 0;
    http2::stream_id next    = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                // a request whose body never comes.
                                                                stalled = c.request("POST", "/echo", "", false);
                                                                const bool reset = co_await c.wait_for(stalled);
                                                                if (!reset) co_return "the stalled one didn't end";

                                                                // the connection goes on.
                                                                next            = c.request("GET", "/");
                                                                const bool done = co_await c.wait_for(next);
                                                                co_return done ? "" : "the next wasn't answered";
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(out.responses[stalled].reset == http2::error_code::cancel);
    CHECK(out.responses[next].status == "200");
}

TEST_CASE("an idle HTTP/2 connection is closed with GOAWAY", "[http][http2][server][timeouts]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, {.header_read_timeout = 1s}};

    seen       out;
    const auto start  = std::chrono::steady_clock::now();
    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                const auto id   = c.request("GET", "/");
                                                                const bool done = co_await c.wait_for(id);
                                                                if (!done) co_return "no response";

                                                                // then nothing, until the server ends it.
                                                                (void)co_await c.pump([&] { return c.closed; });
                                                                co_return std::string{};
                                                            }));
    const auto took   = std::chrono::steady_clock::now() - start;
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(out.closed);
    CHECK(out.goaway);
    CHECK(took < 5s);
}

TEST_CASE("closing a server ends its HTTP/2 connections gracefully", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    seen             out;
    http2::stream_id slow = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                // a request in progress when the server's closed still
                                                                // gets its response.
                                                                // NOTE: the server's SETTINGS say it's serving the
                                                                //       connection (closing the server before it had
                                                                //       accepted it would leave it unanswered in the
                                                                //       listener's queue). It reads the request before
                                                                //       the client's acknowledgement of its PING, so
                                                                //       its final GOAWAY includes it.
                                                                slow                = c.request("GET", "/slow");
                                                                const bool settings = co_await c.pump(
                                                                    [&] { return c.s.has_peer_settings(); });
                                                                if (!settings) co_return "no SETTINGS";

                                                                server.server.close();
                                                                const bool done = co_await c.wait_for(slow);
                                                                if (!done) co_return "no response";

                                                                (void)co_await c.pump([&] { return c.closed; });
                                                                co_return std::string{};
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(out.responses[slow].body == "slow");
    CHECK(out.closed);
    CHECK(out.goaway);
}

TEST_CASE("HTTP/1.1 is still served on the same port", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    const auto test = [](net::io::scheduler* sched, std::string port) -> task<std::string>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        if (!conn.has_value()) co_return "not connected: " + conn.error().message();
        co_await net::io::write_all(*conn, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"sv);
        auto response = co_await net::io::read_all(*conn);
        co_return response.value_or("");
    };

    const auto response = sched.run_to_completion(test(&sched, server.port()));
    CAPTURE(response);
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(response.ends_with("hello over HTTP/1"));
}

TEST_CASE("without http2, a client's HTTP/2 preface isn't understood", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, {.http2 = false}};

    seen             out;
    http2::stream_id id = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                id = c.request("GET", "/");
                                                                (void)co_await c.wait_for(id);
                                                                co_return std::string{};
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK_FALSE(out.responses[id].done);
    CHECK(out.closed);
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

    const std::string upload(300'000, 'u');

    seen                     out;
    std::vector<std::string> statuses; // the ignored bodies' responses'
    http2::stream_id         uploaded = 0;

    const auto failed = sched.run_to_completion(with_client(
        &sched,
        server.port(),
        &out,
        [&](h2_client& c) -> task<std::string>
        {
            // 30 bodies of 60 KB (1.8 MB), each received whole before its handler returns without reading it.
            const std::string body(60'000, 'x');
            for (int i = 0; i < 30; ++i)
            {
                const auto id     = c.request("POST", "/ignore", body);
                const bool synced = co_await c.sync();
                if (!synced) co_return std::format("body {} wasn't received", i);
                g.open();
                const bool done = co_await c.wait_for(id);
                if (!done) co_return std::format("body {} wasn't answered", i);
                statuses.push_back(c.responses[id].status);
            }

            // then an upload, which needs the connection's window.
            uploaded        = c.request("POST", "/echo", upload);
            const bool done = co_await c.wait_for(uploaded);
            co_return done ? "" : "the upload wasn't answered";
        },
        5s));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(statuses == std::vector<std::string>(30, "200"));
    CAPTURE(out.responses[uploaded].reset.message());
    CHECK(out.responses[uploaded].body == upload);
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

    seen             out;
    http2::stream_id id = 0;

    const auto failed = sched.run_to_completion(with_client(
        &sched,
        server.port(),
        &out,
        [&](h2_client& c) -> task<std::string>
        {
            // a stream's window (65535) is sent at once; the rest fits in what the reads give back.
            const std::string body(90'000, 'x');
            id                = c.request("POST", "/partial", body);
            const bool synced = co_await c.sync();
            if (!synced) co_return "the body's window wasn't received";
            g.open();

            const bool sent = co_await c.pump([&] { return c.pending_body.empty(); });
            if (!sent) co_return "the rest of the body wasn't sent";
            g.open();

            const bool done = co_await c.wait_for(id);
            co_return done ? "" : "no response";
        },
        5s));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(out.responses[id].body == "90000");
}

TEST_CASE("an HTTP/2 handler that throws mid-body has its stream reset, rather than its response ended",
          "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    seen             out;
    http2::stream_id with_length    = 0;
    http2::stream_id without_length = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                with_length    = c.request("GET", "/throw-mid-body");
                                                                without_length = c.request("GET", "/throw-mid-stream");
                                                                const bool done =
                                                                    co_await c.pump([&] { return c.finished == 2; });
                                                                co_return done ? "" : "not both ended";
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    // the server's reset, not one for a Content-Length the client found short.
    CHECK(out.responses[with_length].reset == http2::error_code::internal_error);
    CHECK(out.responses[without_length].reset == http2::error_code::internal_error);
}

TEST_CASE("an HTTP/2 handler that throws after an interim response gets a 500, without its headers",
          "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    seen             out;
    http2::stream_id id = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                id = c.request("GET", "/throw-after-interim");
                                                                const bool done = co_await c.wait_for(id);
                                                                co_return done ? "" : "no response";
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    const auto& r = out.responses[id];
    CAPTURE(r.reset.message());
    CHECK(r.status == "500");
    CHECK(header(r, "x-meant-for-200") == "(none)");
    CHECK(header(r, "date") != "(none)");
}

TEST_CASE("WINDOW_UPDATEs on the connection don't put off an HTTP/2 stream's body_read_timeout",
          "[http][http2][server][timeouts]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, {.body_read_timeout = 1s}};

    seen                                out;
    http2::stream_id                    stalled = 0;
    std::chrono::steady_clock::duration took{};

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                // a request whose body never comes, while the
                                                                // connection's window grows every 0.3 s (which wakes
                                                                // the stream's reader).
                                                                const auto start = std::chrono::steady_clock::now();
                                                                stalled = c.request("POST", "/echo", "", false);
                                                                while (!c.responses[stalled].done && !c.closed
                                                                       && std::chrono::steady_clock::now() - start < 4s)
                                                                {
                                                                    (void)co_await c.wait_for(stalled, 300ms);
                                                                    const bool updated = co_await c.window_update(0, 1);
                                                                    if (!updated)
                                                                        co_return "the WINDOW_UPDATE wasn't sent";
                                                                }
                                                                took = std::chrono::steady_clock::now() - start;
                                                                co_return std::string{};
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(out.responses[stalled].reset == http2::error_code::cancel);
    CHECK(took < 3s);
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
        seen             out;
        http2::stream_id second = 0;
        http2::stream_id third  = 0;
        bool             early  = false; // the third was answered while the held ones were

        const auto failed = sched.run_to_completion(with_client(&sched,
                                                                server.port(),
                                                                &out,
                                                                [&](h2_client& c) -> task<std::string>
                                                                {
                                                                    // two handlers held; one's stream reset (which
                                                                    // frees the stream, but not the handler); then
                                                                    // another.
                                                                    const auto first = c.request("GET", "/hold");
                                                                    second           = c.request("GET", "/hold");
                                                                    c.s.reset(first, http2::error_code::cancel);
                                                                    third = c.request("GET", "/");

                                                                    // it waits, though it's quick, until a held one
                                                                    // finishes: by the PING's answer, the server has
                                                                    // its request, and has queued it rather than
                                                                    // started its handler, which would answer at once.
                                                                    const bool synced = co_await c.sync();
                                                                    if (!synced)
                                                                        co_return "the requests weren't received";
                                                                    // NOTE: timed, from then: nothing but its not being
                                                                    //       answered shows the request's still queued.
                                                                    early = co_await c.wait_for(third, 300ms);

                                                                    g.open();
                                                                    const bool answered = co_await c.wait_for(third);
                                                                    if (!answered)
                                                                        co_return "the third wasn't answered";

                                                                    g.open();
                                                                    const bool done = co_await c.wait_for(second);
                                                                    co_return done ? "" : "the second wasn't answered";
                                                                }));
        CAPTURE(failed);
        REQUIRE(failed.empty());

        CHECK_FALSE(early);
        CHECK(out.responses[third].body == "hello over HTTP/2");
        CHECK(out.responses[second].status == "200");
    }

    SECTION("too many waiting, the connection's ended at once, though its handlers hold its slot")
    {
        const auto port = server.port();

        seen             out;
        seen             next_out;
        http2::stream_id next_id = 0;
        bool             closed  = false;
        bool             early   = false; // the next client was answered while the handlers held the slot

        const auto failed = sched.run_to_completion(with_client(
            &sched,
            port,
            &out,
            [&](h2_client& c) -> task<std::string>
            {
                // streams opened and reset at once, whose handlers never finish: the first two run, the rest wait,
                // until the ninth is one too many.
                for (int i = 0; i < 20; ++i)
                {
                    const auto id = c.request("GET", "/hold");
                    c.s.reset(id, http2::error_code::cancel);
                }

                // the connection's closed, though its handlers are held (RFC 9113, section 5.4.1).
                closed = co_await c.pump([&] { return c.closed; }, 3s);
                if (!closed) co_return "the connection wasn't closed";

                // the handlers still hold its slot (max_connections), until they finish.
                co_return co_await with_client(&sched,
                                               port,
                                               &next_out,
                                               [&](h2_client& next) -> task<std::string>
                                               {
                                                   // NOTE: timed: this connection isn't accepted while the slot's
                                                   //       held, so no PING can be answered on it, and nothing shows
                                                   //       it would have been served by now if it were going to be.
                                                   next_id = next.request("GET", "/");
                                                   early   = co_await next.wait_for(next_id, 300ms);

                                                   g.open();
                                                   g.open();
                                                   const bool answered = co_await next.wait_for(next_id);
                                                   co_return answered ? "" : "the next client wasn't answered";
                                               });
            }));
        CAPTURE(failed);
        REQUIRE(failed.empty());

        CHECK(closed);
        CHECK(out.goaway);
        CHECK(out.goaway_error == http2::error_code::enhance_your_calm);

        // the streams after the last started (3) never will be, so they may be retried.
        CHECK(out.goaway_last == 3);

        CHECK_FALSE(early);
        CHECK(next_out.responses[next_id].body == "hello over HTTP/2");
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

    struct result
    {
        std::string                         failed;
        bool                                closed = false;
        std::chrono::steady_clock::duration took{};
        bool                                answered = false; // the held request was
    };

    const auto test = [](net::io::scheduler* sched, std::string port) -> task<result>
    {
        result out;
        auto   conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        if (!conn.has_value())
        {
            out.failed = "not connected: " + conn.error().message();
            co_return out;
        }

        h2_client  c{&*conn, &*conn, &*conn};
        const auto id     = c.request("GET", "/hold");
        const bool synced = co_await c.sync(); // the server has the request (and nothing's left to send)
        if (!synced)
        {
            out.failed = "the request wasn't received";
            co_return out;
        }

        conn->shutdown_write();

        const auto start = std::chrono::steady_clock::now();
        out.closed       = co_await c.pump([&] { return c.closed; }, 3s);
        out.took         = std::chrono::steady_clock::now() - start;
        out.answered     = c.responses[id].done;
        out.failed       = c.failure;
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, server.port()));
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());

    CHECK(out.closed);
    CHECK(out.took < 1s);
    CHECK_FALSE(out.answered);
}

TEST_CASE("an HTTP/2 connection that ends reads what the client still sends, rather than cutting off the response",
          "[http][http2][server]")
{
    // more than TCP's buffers hold while the client doesn't read.
    constexpr std::size_t size = std::size_t{8} << 20;

    // NOTE: before the scheduler, as the handler uses it.
    std::atomic<std::uint64_t> written{0};

    net::io::scheduler sched{make_pool()};

    auto r = make_router(&sched);
    r.GET("/huge",
          [&written, &sched](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              // in one write, which returns once it's all framed (the windows don't hold it back): with the last of
              // it still in the server's output, and in TCP's buffers.
              const std::string body(size, 'h');
              auto*             out = co_await resp.send(http::status::OK, body.size());
              co_await net::io::write_all(*out, body);

              written.store(1);
              sched.notify_all(written);
          });

    test_server server{&sched, std::move(r)};

    struct result
    {
        std::string         failed;
        bool                done = false;
        h2_client::response response;
    };

    const auto test =
        [](net::io::scheduler* sched, std::string port, std::atomic<std::uint64_t>* written) -> task<result>
    {
        result out;
        auto   conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        if (!conn.has_value())
        {
            out.failed = "not connected: " + conn.error().message();
            co_return out;
        }

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
        if (sent.err)
        {
            out.failed = "the request wasn't sent: " + sent.err.message();
            co_return out;
        }

        // the client reads until the handler's returned, with the end of the response still to be sent (about 1
        // MiB, here: what's queued, and what TCP's buffers hold), then stops reading; by then, the connection's ended.
        // NOTE: timed: the client mustn't read (that's the test), so it can't see the connection end; if the PING
        //       below arrives before it does, the test passes, proving less.
        (void)co_await c.pump([&] { return written->load() != 0; });
        (void)co_await sched->sleep_until(net::io::clock::now() + 100ms);

        // the client sends something more (as it sends WINDOW_UPDATEs as it reads): the server mustn't reset the
        // connection for it (e.g. for input after shutting down its reading side, on macOS; or closing with it unread),
        // which would discard what it's still sending.
        constexpr std::array<std::byte, 8> opaque{};
        c.s.ping(opaque);

        out.done     = co_await c.wait_for(id);
        out.response = c.responses[id];
        out.failed   = c.failure;
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, server.port(), &written));
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());

    CHECK(out.done);
    CAPTURE(out.response.reset.message());
    CHECK(out.response.status == "200");
    CHECK(out.response.body.size() == size);
}

TEST_CASE("HTTP/2's header list limit doesn't wrap with max_header_bytes near SIZE_MAX",
          "[http][http2][server][limits]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, {.max_header_bytes = SIZE_MAX}};

    seen             out;
    http2::stream_id id = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                // more than 32 bytes a field for HTTP/1.1's most fields
                                                                // (3,200), which is all a limit that wrapped would
                                                                // allow.
                                                                const std::string                 value(5'000, 'v');
                                                                const std::array<hpack::field, 1> fields{
                                                                    hpack::field{.name = "x-large", .value = value}
                                                                };

                                                                id = c.request("GET", "/", {}, true, fields);
                                                                const bool done = co_await c.wait_for(id);
                                                                co_return done ? "" : "no response";
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    const auto& resp = out.responses[id];
    CAPTURE(resp.reset.message());
    CHECK(resp.status == "200");
    CHECK(resp.body == "hello over HTTP/2");
}

TEST_CASE("an HTTP/2 request over the header list limit gets 431, with no handler, and the connection goes on",
          "[http][http2][server][limits]")
{
    // NOTE: before the scheduler, as the handler uses it.
    std::atomic<int> handled{0};

    net::io::scheduler sched{make_pool()};

    // /fields answers with its x-long's length and its x-marker, counting the handlers that ran.
    http::router router;
    router.GET("/fields",
               [&handled](const http::server_request& req, http::response_writer& resp) -> task<void>
               {
                   ++handled;
                   const auto msg  = std::format("{} {}",
                                                 req.headers.get("x-long").value_or("").size(),
                                                 req.headers.get("x-marker").value_or("(none)"));
                   auto*      body = co_await resp.send(http::status::OK, msg.size());
                   co_await net::io::write_all(*body, msg);
               });

    // a list of at most 1 KiB, and 32 bytes more a field for up to 100 fields: 4,224 bytes.
    test_server server{&sched, std::move(router), {.max_header_bytes = 1'024}};

    seen             out;
    http2::stream_id ended   = 0;
    http2::stream_id unended = 0;
    http2::stream_id id      = 0;

    const std::string value(2'000, 'a');

    const auto failed = sched.run_to_completion(with_client(
        &sched,
        server.port(),
        &out,
        [&](h2_client& c) -> task<std::string>
        {
            // x-long is 2,038 bytes of a list, so three take it past the limit; but the client's HPACK table indexes
            // the first, so the others are an index each, and the block's small (and its value's Huffman coded).
            const std::array<hpack::field, 4> too_many{
                hpack::field{.name = "x-marker", .value = "hello"},
                hpack::field{  .name = "x-long",   .value = value},
                hpack::field{  .name = "x-long",   .value = value},
                hpack::field{  .name = "x-long",   .value = value},
            };

            // one that ends its stream, and one whose body was to follow (it never comes).
            ended               = c.request("GET", "/fields", {}, true, too_many);
            unended             = c.request("POST", "/fields", {}, false, too_many);
            const bool answered = co_await c.pump([&] { return c.responses[ended].done && c.responses[unended].done; });
            if (!answered) co_return "the requests over the limit weren't answered";

            // by the PING's answer, what the server sent before it (the RST_STREAM after the 431) has arrived.
            const bool synced = co_await c.sync();
            if (!synced) co_return "no PING's answer";

            // the connection goes on: a request indexing the fields the long blocks added to HPACK's table is answered
            // as they were sent, so the server's table is in step with the client's.
            const std::array<hpack::field, 2> fields{
                hpack::field{  .name = "x-long",   .value = value},
                hpack::field{.name = "x-marker", .value = "hello"},
            };
            id              = c.request("GET", "/fields", {}, true, fields);
            const bool done = co_await c.wait_for(id);
            co_return done ? "" : "the next request wasn't answered";
        }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    for (const auto stream : {ended, unended})
    {
        CAPTURE(stream);
        const auto& r = out.responses[stream];
        CHECK(r.status == "431");
        CHECK(r.head_ended);
        CHECK(r.body.empty());

        // just its status and Date.
        REQUIRE(r.headers.size() == 1);
        CHECK(r.headers[0].first == "date");
    }
    CHECK_FALSE(out.responses[ended].reset);
    CHECK(out.responses[unended].reset == http2::error_code::no_error);

    const auto& r = out.responses[id];
    CAPTURE(r.reset.message());
    CHECK(r.status == "200");
    CHECK(r.body == "2000 hello");
    CHECK_FALSE(out.goaway);

    // no handler ran for the requests over the limit.
    CHECK(handled == 1);
}

TEST_CASE("an HTTP/2 response's field with a line break throws, as over HTTP/1.1", "[http][http2][server]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    seen             out;
    http2::stream_id id = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                id              = c.request("GET", "/bad-header");
                                                                const bool done = co_await c.wait_for(id);
                                                                co_return done ? "" : "no response";
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    const auto& r = out.responses[id];
    CAPTURE(r.reset.message());
    CHECK(r.status == "200");
    CHECK(r.body == "threw");
    CHECK(header(r, "x-bad") == "(none)");
}

namespace
{

// probe_router is make_router's, with handlers that wait on their request's token:
// - /wait waits, then answers 200 "waited" if it wasn't stopped, or 503 "aborted" if the server was aborted, or
//   "stopped" if it was stopped otherwise;
// - /wait-silent waits, then returns without answering;
// - /partial sends a head (with a Content-Length of 100) and some of its body, then waits;
// - /nap waits a little (or until stopped), then answers "rested" or "stopped";
// - /doze is /nap, but waits a minute (or until stopped): for a test that stops it, however late.
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

    const auto nap = [sched, probe](net::io::clock::duration length)
    {
        return [sched, probe, length](const http::server_request& req, http::response_writer& resp) -> task<void>
        {
            stop_probe::bump(sched, probe->started);
            const auto err = co_await sched->sleep_until(net::io::clock::now() + length, req.stop);
            const auto msg = err ? "stopped"sv : "rested"sv;
            stop_probe::bump(sched, probe->woke);

            auto* body = co_await resp.send(http::status::OK, msg.size());
            co_await net::io::write_all(*body, msg);
        };
    };
    r.GET("/nap", nap(300ms));
    r.GET("/doze", nap(60s));
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

    seen                                out;
    http2::stream_id                    other = 0;
    http2::stream_id                    next  = 0;
    std::chrono::steady_clock::duration took{};

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            &out,
                                            [&](h2_client& c) -> task<std::string>
                                            {
                                                const auto reset  = c.request("GET", "/wait");
                                                other             = c.request("GET", "/slow");
                                                const bool synced = co_await c.sync();
                                                if (!synced) co_return "the requests weren't received";
                                                const bool started =
                                                    co_await stop_probe::reach(&sched, probe.started, 1);
                                                if (!started) co_return "the handler didn't start";

                                                const auto start = std::chrono::steady_clock::now();
                                                c.s.reset(reset, http2::error_code::cancel);

                                                // the other stream goes on.
                                                const bool answered = co_await c.wait_for(other);
                                                if (!answered) co_return "the other wasn't answered";

                                                const bool woke = co_await stop_probe::reach(&sched, probe.woke, 1);
                                                if (!woke) co_return "the handler didn't wake";
                                                took = std::chrono::steady_clock::now() - start;

                                                // and the connection.
                                                next            = c.request("GET", "/");
                                                const bool done = co_await c.wait_for(next);
                                                co_return done ? "" : "the next wasn't answered";
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(out.responses[other].body == "slow");
    CHECK(probe.canceled.load() == 1);
    CHECK(took < 5s);
    CHECK(out.responses[next].status == "200");
}

TEST_CASE("an HTTP/2 handler's token is stopped when its stream times out", "[http][http2][server][cancel]")
{
    // NOTE: before the scheduler, as the handlers use them.
    stop_probe                 probe;
    std::atomic<std::uint64_t> read_done{0};
    std::atomic_bool           spawn_failed{false}; // the handler couldn't spawn its reader
    std::atomic_bool           read_late{false};    // the reader ended long after the handler was stopped

    net::io::scheduler sched{make_pool()};

    // reads its body in a task of its own (which times out, as the body never comes), while it waits on its token.
    auto r = probe_router(&sched, &probe);
    r.POST("/read-aside",
           [&](http::server_request& req, http::response_writer&) -> task<void>
           {
               const bool spawned = sched.spawn(
                   [](net::io::scheduler* sched, net::io::reader* body, std::atomic<std::uint64_t>* done) -> task<>
                   {
                       (void)co_await net::io::read_all(*body);
                       stop_probe::bump(sched, *done);
                   }(&sched, req.body.get(), &read_done));
               if (!spawned) spawn_failed = true;

               (void)co_await probe.wait(&sched, req.stop);
               if (!spawned) co_return;

               // the body's reader is the stream's, which goes once the handler returns: so the handler waits for the
               // task reading it. Should that be late, it still waits (the read ends with the stream, at the latest
               // when the connection ends as the scheduler stops), so the reader never reads a body that's gone.
               if (co_await stop_probe::reach(&sched, read_done, 1)) co_return;
               read_late = true;
               for (auto seen = read_done.load(); seen < 1; seen = read_done.load())
                   (void)co_await sched.wait_on(read_done, seen);
           });

    test_server server{&sched, std::move(r), {.body_read_timeout = 1s}};

    seen             out;
    http2::stream_id stalled = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                stalled = c.request("POST", "/read-aside", "", false);
                                                                const bool reset = co_await c.wait_for(stalled);
                                                                if (!reset) co_return "the stream didn't end";

                                                                const bool woke =
                                                                    co_await stop_probe::reach(&sched, probe.woke, 1);
                                                                co_return woke ? "" : "the handler didn't wake";
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK_FALSE(spawn_failed);
    CHECK_FALSE(read_late);
    CHECK(out.responses[stalled].reset == http2::error_code::cancel);
    CHECK(probe.canceled.load() == 1);
}

TEST_CASE("an HTTP/2 handler's token is stopped when its connection ends", "[http][http2][server][cancel]")
{
    // NOTE: before the scheduler, as the handlers use it.
    stop_probe probe;

    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, probe_router(&sched, &probe)};

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            nullptr,
                                            [&](h2_client& c) -> task<std::string>
                                            {
                                                (void)c.request("GET", "/wait");
                                                (void)c.request("GET", "/wait");
                                                const bool synced = co_await c.sync();
                                                if (!synced) co_return "the requests weren't received";
                                                const bool started =
                                                    co_await stop_probe::reach(&sched, probe.started, 2);
                                                if (!started) co_return "the handlers didn't start";

                                                // the client closes its side.
                                                c.sock->shutdown_write();

                                                const bool woke = co_await stop_probe::reach(&sched, probe.woke, 2);
                                                co_return woke ? "" : "the handlers didn't wake";
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(probe.canceled.load() == 2);
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

    seen             out;
    seen             next_out;
    http2::stream_id next_id = 0;

    const auto failed = sched.run_to_completion(
        with_client(&sched,
                    port,
                    &out,
                    [&](h2_client& c) -> task<std::string>
                    {
                        // as the group-5 test: streams opened and reset at once, until the connection's ended with
                        // ENHANCE_YOUR_CALM.
                        for (int i = 0; i < 20; ++i)
                        {
                            const auto id = c.request("GET", "/hold");
                            c.s.reset(id, http2::error_code::cancel);
                        }

                        const bool closed = co_await c.pump([&] { return c.closed; }, 3s);
                        if (!closed) co_return "the connection wasn't closed";

                        // the handlers return (their tokens stopped), so the next client's served without the gate
                        // opening.
                        co_return co_await with_client(&sched,
                                                       port,
                                                       &next_out,
                                                       [&](h2_client& next) -> task<std::string>
                                                       {
                                                           next_id             = next.request("GET", "/");
                                                           const bool answered = co_await next.wait_for(next_id, 3s);
                                                           co_return answered ? "" : "the next client wasn't answered";
                                                       });
                    }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(out.goaway_error == http2::error_code::enhance_your_calm);
    CHECK(next_out.responses[next_id].body == "hello over HTTP/2");
}

TEST_CASE("the client's GOAWAY doesn't stop the HTTP/2 handlers running", "[http][http2][server][cancel]")
{
    // NOTE: before the scheduler, as the handlers use it.
    stop_probe probe;

    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, probe_router(&sched, &probe)};

    seen             out;
    http2::stream_id id = 0;

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            &out,
                                            [&](h2_client& c) -> task<std::string>
                                            {
                                                id                = c.request("GET", "/nap");
                                                const bool synced = co_await c.sync();
                                                if (!synced) co_return "the request wasn't received";
                                                const bool started =
                                                    co_await stop_probe::reach(&sched, probe.started, 1);
                                                if (!started) co_return "the handler didn't start";

                                                c.s.goaway(0, http2::error_code::no_error);
                                                const bool done = co_await c.wait_for(id);
                                                co_return done ? "" : "no response";
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(out.responses[id].body == "rested");
}

TEST_CASE("closing the server doesn't stop HTTP/2 handlers' tokens; aborting it does", "[http][http2][server][cancel]")
{
    // NOTE: before the scheduler, as the handlers use it.
    stop_probe probe;

    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, probe_router(&sched, &probe)};

    // starts a request for path, and waits for its handler to start, returning its stream, or what went wrong.
    const auto start = [&](h2_client& c, std::string_view path) -> task<std::expected<http2::stream_id, std::string>>
    {
        const auto id     = c.request("GET", path);
        const bool synced = co_await c.sync();
        if (!synced) co_return std::unexpected("the request wasn't received");
        const bool started = co_await stop_probe::reach(&sched, probe.started, 1);
        if (!started) co_return std::unexpected("the handler didn't start");
        co_return id;
    };

    seen out;

    SECTION("closed: the handler finishes")
    {
        http2::stream_id id     = 0;
        const auto       failed = sched.run_to_completion(with_client(&sched,
                                                                      server.port(),
                                                                      &out,
                                                                      [&](h2_client& c) -> task<std::string>
                                                                      {
                                                                    const auto started = co_await start(c, "/nap");
                                                                    if (!started) co_return started.error();
                                                                    id = *started;
                                                                    server.server.close();

                                                                    const bool done = co_await c.wait_for(id);
                                                                    co_return done ? "" : "no response";
                                                                      }));
        CAPTURE(failed);
        REQUIRE(failed.empty());

        CHECK(out.responses[id].body == "rested");
    }

    SECTION("aborted: a handler may still answer")
    {
        http2::stream_id id     = 0;
        const auto       failed = sched.run_to_completion(with_client(&sched,
                                                                      server.port(),
                                                                      &out,
                                                                      [&](h2_client& c) -> task<std::string>
                                                                      {
                                                                    const auto started = co_await start(c, "/wait");
                                                                    if (!started) co_return started.error();
                                                                    id = *started;
                                                                    server.server.abort();

                                                                    const bool done = co_await c.wait_for(id);
                                                                    co_return done ? "" : "no response";
                                                                      }));
        CAPTURE(failed);
        REQUIRE(failed.empty());

        CHECK(out.responses[id].status == "503");
        CHECK(out.responses[id].body == "aborted");
        CHECK(probe.canceled.load() == 1);
    }

    SECTION("aborted: an incomplete response is reset with CANCEL")
    {
        http2::stream_id id     = 0;
        const auto       failed = sched.run_to_completion(with_client(&sched,
                                                                      server.port(),
                                                                      &out,
                                                                      [&](h2_client& c) -> task<std::string>
                                                                      {
                                                                    const auto started = co_await start(c, "/partial");
                                                                    if (!started) co_return started.error();
                                                                    id = *started;
                                                                    server.server.abort();

                                                                    const bool done = co_await c.wait_for(id);
                                                                    co_return done ? "" : "no response";
                                                                      }));
        CAPTURE(failed);
        REQUIRE(failed.empty());

        CHECK(out.responses[id].status == "200");
        CHECK(out.responses[id].body == "partial");
        CHECK(out.responses[id].reset == http2::error_code::cancel);
    }

    SECTION("aborted: a handler that doesn't answer gets no answer from the server")
    {
        http2::stream_id id     = 0;
        const auto       failed = sched.run_to_completion(with_client(&sched,
                                                                      server.port(),
                                                                      &out,
                                                                      [&](h2_client& c) -> task<std::string>
                                                                      {
                                                                    const auto started =
                                                                        co_await start(c, "/wait-silent");
                                                                    if (!started) co_return started.error();
                                                                    id = *started;
                                                                    server.server.abort();

                                                                    const bool done = co_await c.wait_for(id);
                                                                    co_return done ? "" : "no reset";
                                                                      }));
        CAPTURE(failed);
        REQUIRE(failed.empty());

        CHECK(out.responses[id].status.empty());
        CHECK(out.responses[id].reset == http2::error_code::cancel);
    }

    SECTION("aborted: a stream started after is stopped from the start")
    {
        http2::stream_id first  = 0;
        http2::stream_id later  = 0;
        const auto       failed = sched.run_to_completion(with_client(&sched,
                                                                      server.port(),
                                                                      &out,
                                                                      [&](h2_client& c) -> task<std::string>
                                                                      {
                                                                    // (a handler that waits a minute, so it's still
                                                                    // waiting when the server's aborted.)
                                                                    const auto started = co_await start(c, "/doze");
                                                                    if (!started) co_return started.error();
                                                                    first = *started;
                                                                    server.server.abort();

                                                                    // (the drain's first GOAWAY still lets the client
                                                                    // open streams.)
                                                                    later               = c.request("GET", "/wait");
                                                                    const bool answered = co_await c.wait_for(later);
                                                                    if (!answered)
                                                                        co_return "the later one wasn't answered";

                                                                    const bool done = co_await c.wait_for(first);
                                                                    co_return done ? "" : "the first wasn't answered";
                                                                      }));
        CAPTURE(failed);
        REQUIRE(failed.empty());

        CHECK(out.responses[later].status == "503");
        CHECK(out.responses[later].body == "aborted");
        CHECK(out.responses[first].body == "stopped");
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

    seen                out;
    http2::stream_id    id = 0;
    h2_client::response hinted; // as it was once both hints had come

    const auto failed = sched.run_to_completion(
        with_client(&sched,
                    server.port(),
                    &out,
                    [&](h2_client& c) -> task<std::string>
                    {
                        id              = c.request("GET", "/hint");
                        const bool came = co_await c.pump([&] { return c.responses[id].interims.size() == 2; });
                        if (!came) co_return "the hints didn't come";
                        hinted = c.responses[id];

                        g.open();
                        const bool finished = co_await c.wait_for(id);
                        co_return finished ? "" : "no response";
                    }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    REQUIRE(hinted.interims.size() == 2);
    CHECK(hinted.interims[0].status == "103");
    CHECK(header(hinted.interims[0], "link") == "</style.css>; rel=preload");
    CHECK(header(hinted.interims[0], "content-length") == "(none)");
    CHECK(hinted.interims[1].status == "103");
    CHECK(hinted.interims[1].headers.size() == hinted.interims[0].headers.size() + 1);

    // not the end of the stream, nor the final response.
    CHECK_FALSE(hinted.done);
    CHECK(hinted.status.empty());

    const auto& final_head = out.responses[id];
    CAPTURE(final_head.reset.message());
    CHECK(final_head.status == "200");
    CHECK(header(final_head, "link") == "</style.css>; rel=preload");
    CHECK(final_head.body == "done");
    CHECK(final_head.interims.size() == 2);
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

    seen             out;
    http2::stream_id id = 0;

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            &out,
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                id                  = c.request("GET", "/switch");
                                                                const bool finished = co_await c.wait_for(id);
                                                                co_return finished ? "" : "no response";
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    const auto& resp = out.responses[id];
    CHECK(resp.interims.empty());
    CHECK(resp.status == "200");
    CHECK(resp.body == "refused");
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

    seen             out;
    http2::stream_id id = 0;

    SECTION("only once the handler reads, before the client sends the body")
    {
        bool early = false; // a 100 came before the handler read

        const auto failed = sched.run_to_completion(
            with_client(&sched,
                        server.port(),
                        &out,
                        [&](h2_client& c) -> task<std::string>
                        {
                            // (the request goes with the PING.)
                            id              = c.request("POST", "/wait", {}, false, expect);
                            const bool sent = co_await c.sync();
                            if (!sent) co_return "the request wasn't received";
                            const bool in = co_await stop_probe::reach(&sched, started, 1);
                            if (!in) co_return "the handler didn't start";

                            // by the PING's answer, whatever the server sent before it is here.
                            const bool synced = co_await c.sync();
                            if (!synced) co_return "no PING's answer";
                            early = !c.responses[id].interims.empty();

                            g.open();
                            const bool asked = co_await c.pump([&] { return !c.responses[id].interims.empty(); });
                            if (!asked) co_return "no 100";

                            c.send(id, "hello", true);
                            const bool finished = co_await c.wait_for(id);
                            co_return finished ? "" : "no response";
                        }));
        CAPTURE(failed);
        REQUIRE(failed.empty());

        CHECK_FALSE(early);

        const auto& resp = out.responses[id];
        CAPTURE(resp.reset.message());
        REQUIRE_FALSE(resp.interims.empty());

        // just the status.
        const auto& cont = resp.interims.front();
        CHECK(cont.status == "100");
        CHECK(cont.headers.empty());

        CHECK(resp.status == "200");
        CHECK(resp.body == "hello");
        CHECK(resp.interims.size() == 1);
    }

    SECTION("one after the handler's Early Hints, which don't stop it")
    {
        h2_client::response asked_for; // as it was once both interim heads had come

        const auto failed = sched.run_to_completion(
            with_client(&sched,
                        server.port(),
                        &out,
                        [&](h2_client& c) -> task<std::string>
                        {
                            id               = c.request("POST", "/hint", {}, false, expect);
                            const bool asked = co_await c.pump([&] { return c.responses[id].interims.size() == 2; });
                            if (!asked) co_return "no 103 and 100";
                            asked_for = c.responses[id];

                            c.send(id, "hello", true);
                            const bool finished = co_await c.wait_for(id);
                            co_return finished ? "" : "no response";
                        }));
        CAPTURE(failed);
        REQUIRE(failed.empty());

        // the 103, then the 100 (just the status), before the body's sent.
        REQUIRE(asked_for.interims.size() == 2);
        CHECK(asked_for.interims[0].status == "103");
        CHECK(header(asked_for.interims[0], "link") == "</style.css>; rel=preload");
        CHECK(asked_for.interims[1].status == "100");
        CHECK(asked_for.interims[1].headers.empty());
        CHECK_FALSE(asked_for.done);
        CHECK(asked_for.status.empty());

        const auto& resp = out.responses[id];
        CAPTURE(resp.reset.message());
        CHECK(resp.status == "200");
        CHECK(header(resp, "link") == "</style.css>; rel=preload");
        CHECK(resp.body == "hello");
        CHECK(resp.interims.size() == 2);
    }

    SECTION("none for a handler that answers without reading")
    {
        const auto failed = sched.run_to_completion(with_client(&sched,
                                                                server.port(),
                                                                &out,
                                                                [&](h2_client& c) -> task<std::string>
                                                                {
                                                                    id =
                                                                        c.request("POST", "/refuse", {}, false, expect);
                                                                    const bool finished = co_await c.wait_for(id);
                                                                    co_return finished ? "" : "no response";
                                                                }));
        CAPTURE(failed);
        REQUIRE(failed.empty());

        const auto& resp = out.responses[id];
        CHECK(resp.status == "413");
        CHECK(resp.interims.empty());
    }

    SECTION("417 for another expectation, without running the handler")
    {
        const auto failed =
            sched.run_to_completion(with_client(&sched,
                                                server.port(),
                                                &out,
                                                [&](h2_client& c) -> task<std::string>
                                                {
                                                    const std::array other{
                                                        hpack::field{.name = "expect", .value = "something"}
                                                    };
                                                    id = c.request("POST", "/wait", "hello", true, other);
                                                    const bool finished = co_await c.wait_for(id);
                                                    co_return finished ? "" : "no response";
                                                }));
        CAPTURE(failed);
        REQUIRE(failed.empty());

        const auto& resp = out.responses[id];
        CHECK(resp.status == "417");
        CHECK(resp.interims.empty());
        CHECK(started.load() == 0);
    }
}

namespace
{

// indexing_router answers with fields HPACK's table can hold: some that repeat, the request's own sent back, and a
// cookie (never indexed); and at /big/:name, a field of 3000 bytes named for name, near 3/4 of a table of 4096.
http::router indexing_router()
{
    http::router r;
    r.GET("/fields",
          [](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              resp.headers()
                  .set("Content-Type", "text/plain")
                  .set("X-Served-By", "cppnet's tests")
                  .set("X-Token-Seen", req.headers.get("x-token").value_or("(none)"))
                  .set("X-Cookie-Seen", req.headers.get("cookie").value_or("(none)"))
                  .set("Set-Cookie", "session=secret");
              auto* body = co_await resp.send(http::status::OK, 2);
              co_await net::io::write_all(*body, "ok"sv);
          });
    r.GET("/big/:name",
          [](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              const auto name = req.path_value("name");
              resp.headers().set(std::format("X-{}", name), std::string(3'000, name.front()));
              co_await resp.send(http::status::NO_CONTENT);
          });
    return r;
}

}

TEST_CASE("the server's HTTP/2 responses repeat fields as indexes of HPACK's table, but never cookies",
          "[http][http2][server][hpack]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, indexing_router()};

    seen                          out;
    std::vector<http2::stream_id> ids;

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            &out,
                                            [&](h2_client& c) -> task<std::string>
                                            {
                                                // the client's own session indexes too: x-token, and
                                                // the server's table copes; the cookie it doesn't.
                                                const std::array fields{
                                                    hpack::field{.name = "x-token",   .value = "abc"},
                                                    hpack::field{ .name = "cookie", .value = "id=42"}
                                                };

                                                for (int i = 0; i < 3; ++i)
                                                {
                                                    ids.push_back(c.request("GET", "/fields", {}, true, fields));
                                                    const bool finished = co_await c.wait_for(ids.back());
                                                    if (!finished)
                                                        co_return std::format("request {} wasn't answered", i);
                                                }
                                                co_return std::string{};
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    for (const auto id : ids)
    {
        CAPTURE(id);
        const auto& r = out.responses[id];
        CHECK(r.status == "200");
        CHECK(r.body == "ok");
        CHECK(header(r, "content-type") == "text/plain");
        CHECK(header(r, "x-served-by") == "cppnet's tests");
        CHECK(header(r, "x-token-seen") == "abc");
        CHECK(header(r, "x-cookie-seen") == "id=42");
        CHECK(header(r, "set-cookie") == "session=secret");
        CHECK(r.never_indexed == std::vector<std::string>{"set-cookie"});
        REQUIRE(out.blocks[id].size() == 1);
        CHECK(test::size_updates(out.blocks[id][0]).empty());
    }

    // the fields the first response put in the table are indexes in the next: the second's block is smaller (by more
    // than the cookie, which is a literal each time), and so is the third's (the date may have changed for the second,
    // and been added again).
    const auto first  = out.blocks[ids[0]][0].size();
    const auto second = out.blocks[ids[1]][0].size();
    const auto third  = out.blocks[ids[2]][0].size();
    CAPTURE(first, second, third);
    CHECK(second + 20 < first);
    CHECK(third + 20 < first);
}

TEST_CASE("a client's SETTINGS_HEADER_TABLE_SIZE above 4096 leaves the server's HPACK table at 4096",
          "[http][http2][server][hpack]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, indexing_router()};

    http2::session_config config;
    config.local.header_table_size = 65'536;

    seen                          out;
    std::vector<http2::stream_id> ids;

    const auto failed = sched.run_to_completion(with_client(
        &sched,
        server.port(),
        &out,
        [&](h2_client& c) -> task<std::string>
        {
            // x-a, then x-b, then x-a again: in a table of 4096, x-b pushes x-a out, so it's sent again as a literal
            // (of about 1900 bytes, Huffman coded); in one of 65536, it would be an index.
            for (const auto* path : {"/big/a", "/big/b", "/big/a"})
            {
                ids.push_back(c.request("GET", path));
                const bool finished = co_await c.wait_for(ids.back());
                if (!finished) co_return std::format("{} wasn't answered", path);
            }
            co_return std::string{};
        },
        std::nullopt,
        config));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    for (const auto id : ids)
    {
        CAPTURE(id);
        CHECK(out.responses[id].status == "204");
        REQUIRE(out.blocks[id].size() == 1);
        CHECK(test::size_updates(out.blocks[id][0]).empty()); // the table's size is still the 4096 the client knows
    }
    CHECK(header(out.responses[ids[2]], "x-a") == std::string(3'000, 'a'));
    CHECK(out.blocks[ids[2]][0].size() > 1'000);
}

TEST_CASE("a client's SETTINGS_HEADER_TABLE_SIZE lowered then raised: the server's next block signals both",
          "[http][http2][server][hpack]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, indexing_router()};

    seen             out;
    http2::stream_id first  = 0;
    http2::stream_id second = 0;
    http2::stream_id third  = 0;

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            &out,
                                            [&](h2_client& c) -> task<std::string>
                                            {
                                                first           = c.request("GET", "/fields");
                                                const bool done = co_await c.wait_for(first);
                                                if (!done) co_return "the first wasn't answered";

                                                // SETTINGS lowering the table to 100, then raising it past the cap,
                                                // written by hand (the session can't change its settings): the server's
                                                // next block must start with 100, then its cap, 4096 (RFC 7541, section
                                                // 4.2), whose decoder (this client's, at 4096 still) accepts them. The
                                                // PING makes sure they've arrived first.
                                                std::vector<std::byte> frames;
                                                http2::encode_settings({.header_table_size = 100}, {}, frames);
                                                http2::encode_settings({.header_table_size = 8'192}, {}, frames);
                                                const auto written = co_await net::io::write_all(*c.out, frames);
                                                if (written.err)
                                                    co_return "the SETTINGS weren't sent: " + written.err.message();
                                                const bool synced = co_await c.sync();
                                                if (!synced) co_return "no PING's answer";

                                                second           = c.request("GET", "/fields");
                                                const bool done2 = co_await c.wait_for(second);
                                                if (!done2) co_return "the second wasn't answered";
                                                third            = c.request("GET", "/fields");
                                                const bool done3 = co_await c.wait_for(third);
                                                co_return done3 ? "" : "the third wasn't answered";
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    for (const auto id : {first, second, third})
    {
        CAPTURE(id);
        CHECK(out.responses[id].status == "200");
        CHECK(header(out.responses[id], "x-served-by") == "cppnet's tests");
        REQUIRE(out.blocks[id].size() == 1);
    }

    CHECK(test::size_updates(out.blocks[first][0]).empty());
    CHECK(test::size_updates(out.blocks[second][0]) == "3f453fe11f"); // 100, then 4096
    CHECK(test::size_updates(out.blocks[third][0]).empty());

    // the table was cut to 100, keeping at most its newest field, so the second response indexed the others again, and
    // the third has them as indexes.
    CHECK(out.blocks[third][0].size() + 20 < out.blocks[second][0].size());
}
