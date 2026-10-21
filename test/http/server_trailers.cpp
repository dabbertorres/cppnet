// The server's trailers (docs/plans/http2.md, stage 5, part 4): a request's, which handlers read once they've read its
// body, and a response's, which response_writer::trailers() sends after its body, over HTTP/1.1 and HTTP/2.
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
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
#include "http/http2.hpp"
#include "http/middleware.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "tcp.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;
namespace hpack = net::http::hpack;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::h2_client;
using test::header;
using test::trailer;

namespace
{

// describe is a request's body and trailers, as the echo handler answers: "body|name=value,value;...", and "early "
// first if the trailers were there before the body had been read to its end.
std::string describe(bool early, std::string_view body, const http::headers& trailers)
{
    std::string out = early ? "early " : "";
    out.append(body).append("|");
    for (const auto& [name, values] : trailers)
    {
        out.append(name).append("=");
        bool first = true;
        for (const auto& value : values)
        {
            if (!first) out.append(",");
            out.append(value);
            first = false;
        }
        out.append(";");
    }
    return out;
}

// set_trailers sets a response's trailers, two that may be and two that can't (which are dropped).
void set_trailers(http::response_writer& resp)
{
    resp.trailers().set("Server-Timing", "db;dur=53");
    resp.trailers().set("Content-Length", "5");
    resp.trailers().set("If-Match", "\"x\"");
    resp.trailers().set("Grpc-Status", "0");
}

http::router make_router(net::io::scheduler* sched)
{
    http::router r;

    // answers with the request's body and trailers (see describe()), chunked, and the trailers sent back as the
    // response's. It waits a moment once it's read the body, so an HTTP/1.1 connection's watched meanwhile.
    r.POST("/echo",
           [sched](http::server_request& req, http::response_writer& resp) -> task<void>
           {
               const bool early = !req.trailers.empty();
               auto       body  = co_await net::io::read_all(*req.body);
               if (!body.has_value()) throw std::system_error{body.error()};

               (void)co_await sched->sleep_until(net::io::clock::now() + 1ms, req.stop);

               const auto msg  = describe(early, *body, req.trailers);
               resp.trailers() = req.trailers;
               auto* out       = co_await resp.send(http::status::OK);
               co_await net::io::write_all(*out, msg);
           });

    // a body of unknown length (chunked, over HTTP/1.1), then trailers.
    r.GET("/chunked",
          [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          {
              auto* body = co_await resp.send(http::status::OK);
              co_await net::io::write_all(*body, "hello"sv);
              set_trailers(resp);
          });

    // a body with a Content-Length, then trailers: dropped over HTTP/1.1.
    r.GET("/length",
          [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          {
              auto* body = co_await resp.send(http::status::OK, 5);
              co_await net::io::write_all(*body, "hello"sv);
              set_trailers(resp);
          });

    // trailers, but no body.
    r.GET("/empty",
          [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          {
              set_trailers(resp);
              co_await resp.send(http::status::OK, 0);
          });

    // no body, and trailers declared, but none set.
    r.GET("/declared",
          [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          {
              resp.headers().set("Trailer", "Grpc-Status");
              co_await resp.send(http::status::OK, 0);
          });

    // a trailer that can't be sent, after some of the body.
    r.GET("/invalid",
          [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          {
              auto* body = co_await resp.send(http::status::OK);
              co_await net::io::write_all(*body, "partial"sv);
              resp.trailers().set("Grpc-Status", "0");
              resp.trailers().set("Grpc-Message", "a\r\nb");
          });

    // trailers, then a throw: the 500 has none.
    r.GET("/throw",
          [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          {
              set_trailers(resp);
              throw std::runtime_error{"oops"};
          });

    return r;
}

// served_by is middleware that adds a trailer once the rest of the chain has answered.
struct served_by
{
    template<class Next>
    task<void> operator()(http::server_request& req, http::response_writer& resp, Next& next) const
    {
        co_await next(req, resp);
        resp.trailers().set("X-Served-By", "middleware");
    }
};

// exchange sends request to the server on port as it is, and reads until the server closes the connection.
task<std::string> exchange(net::io::scheduler* sched, std::uint16_t port, std::string request)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    if (!conn.has_value()) co_return "error: " + conn.error().message();
    conn->set_read_timeout(5s);

    auto res = co_await net::io::write_all(*conn, request);
    if (res.err) co_return "error: " + res.err.message();

    std::string                out;
    std::array<std::byte, 256> buf{};
    while (true)
    {
        auto read = co_await conn->read(buf);
        out.append(reinterpret_cast<const char*>(buf.data()), read.count);
        if (read.err) break;
    }

    co_return out;
}

// chunk is msg as one chunk of a chunked body.
std::string chunk(std::string_view msg) { return std::format("{:x}\r\n{}\r\n", msg.size(), msg); }

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// with_client connects to port, and runs test with an h2_client over the connection.
template<typename Test>
task<> with_client(net::io::scheduler* sched, std::uint16_t port, Test test)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    REQUIRE(conn.has_value());

    h2_client client{&*conn, &*conn, &*conn};
    co_await test(client);
}

}

TEST_CASE("an HTTP/1.1 handler reads a chunked request's trailers once it's read its body", "[http][server][trailers]")
{
    net::io::scheduler sched{make_pool()};
    http::server       server{
        &sched,
        make_router(&sched),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    // a field that can't be a trailer is dropped; a repeated one is one field, with each value.
    const auto out = sched.run_to_completion(
        exchange(&sched,
                 server.local_port(),
                 "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
                 "Connection: close\r\n\r\n"
                 "5\r\nhello\r\n0\r\nX-Checksum: abc\r\nIf-Match: \"x\"\r\nX-Checksum: def\r\n\r\n"));
    CAPTURE(out);

    CHECK(out.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(out.ends_with("\r\n\r\n" + chunk("hello|X-Checksum=abc,def;")
                        + "0\r\nX-Checksum: abc\r\nX-Checksum: def\r\n\r\n"));
}

TEST_CASE("an HTTP/1.1 response's trailers follow a chunked body, and only that", "[http][server][trailers]")
{
    net::io::scheduler sched{make_pool()};
    http::server       server{
        &sched,
        make_router(&sched),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    const auto port = server.local_port();

    SECTION("in the last chunk, without those that can't be trailers")
    {
        const auto out = sched.run_to_completion(
            exchange(&sched, port, "GET /chunked HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
        CAPTURE(out);
        CHECK(out.contains("\r\nTransfer-Encoding: chunked\r\n"));
        CHECK(out.ends_with("\r\n\r\n5\r\nhello\r\n0\r\nServer-Timing: db;dur=53\r\nGrpc-Status: 0\r\n\r\n"));
        CHECK_FALSE(out.contains("If-Match"));
    }

    SECTION("none after a body with a Content-Length")
    {
        const auto out = sched.run_to_completion(
            exchange(&sched, port, "GET /length HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
        CAPTURE(out);
        CHECK(out.contains("\r\nContent-Length: 5\r\n"));
        CHECK(out.ends_with("\r\n\r\nhello"));
        CHECK_FALSE(out.contains("Server-Timing"));
    }

    SECTION("none without a body")
    {
        const auto out = sched.run_to_completion(
            exchange(&sched, port, "GET /empty HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
        CAPTURE(out);
        CHECK(out.contains("\r\nContent-Length: 0\r\n"));
        CHECK(out.ends_with("\r\n\r\n"));
        CHECK_FALSE(out.contains("Server-Timing"));
    }

    SECTION("none to an HTTP/1.0 client, whose body ends with the connection")
    {
        const auto out = sched.run_to_completion(exchange(&sched, port, "GET /chunked HTTP/1.0\r\n\r\n"));
        CAPTURE(out);
        CHECK(out.starts_with("HTTP/1.0 200 OK\r\n"));
        CHECK(out.ends_with("\r\n\r\nhello"));
        CHECK_FALSE(out.contains("Server-Timing"));
    }

    SECTION("none for HEAD")
    {
        const auto out = sched.run_to_completion(
            exchange(&sched, port, "HEAD /chunked HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
        CAPTURE(out);
        CHECK(out.contains("\r\nTransfer-Encoding: chunked\r\n"));
        CHECK(out.ends_with("\r\n\r\n"));
        CHECK_FALSE(out.contains("Server-Timing"));
        CHECK_FALSE(out.contains("\r\n0\r\n"));
    }

    SECTION("an invalid one cuts the response short: the connection closes without the last chunk")
    {
        const auto out = sched.run_to_completion(
            exchange(&sched, port, "GET /invalid HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
        CAPTURE(out);
        CHECK(out.ends_with("\r\n\r\n7\r\npartial\r\n"));
        CHECK_FALSE(out.contains("Grpc"));
    }

    SECTION("none with the 500 for a handler that throws")
    {
        const auto out = sched.run_to_completion(exchange(&sched, port, "GET /throw HTTP/1.1\r\nHost: x\r\n\r\n"));
        CAPTURE(out);
        CHECK(out.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
        CHECK_FALSE(out.contains("Server-Timing"));
    }
}

TEST_CASE("an HTTP/1.1 connection is reused after a response with trailers, and the next request has none",
          "[http][server][trailers]")
{
    net::io::scheduler sched{make_pool()};
    http::server       server{
        &sched,
        make_router(&sched),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    const auto out =
        sched.run_to_completion(exchange(&sched,
                                         server.local_port(),
                                         "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                                         "2\r\nhi\r\n0\r\nX-Checksum: abc\r\n\r\n"
                                         "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n"
                                         "Connection: close\r\n\r\nbye"));
    CAPTURE(out);

    const auto second = out.find("HTTP/1.1 200 OK\r\n", 1);
    REQUIRE(out.starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(second != std::string::npos);

    const auto first = std::string_view{out}.substr(0, second);
    CHECK(first.ends_with("\r\n\r\n" + chunk("hi|X-Checksum=abc;") + "0\r\nX-Checksum: abc\r\n\r\n"));
    CHECK(out.ends_with("\r\n\r\n" + chunk("bye|") + "0\r\n\r\n"));
}

TEST_CASE("middleware adds an HTTP/1.1 response's trailers after the handler", "[http][server][trailers]")
{
    net::io::scheduler sched{make_pool()};
    http::server       server{
        &sched,
        http::chain(served_by{},
        make_router(&sched)),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    const auto out = sched.run_to_completion(
        exchange(&sched, server.local_port(), "GET /chunked HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    CAPTURE(out);
    CHECK(out.ends_with("\r\n0\r\nServer-Timing: db;dur=53\r\nGrpc-Status: 0\r\nX-Served-By: middleware\r\n\r\n"));
}

TEST_CASE("an HTTP/2 handler reads a request's trailers once it's read its body", "[http][http2][server][trailers]")
{
    net::io::scheduler sched{make_pool()};
    http::server       server{
        &sched,
        make_router(&sched),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    sched.run_to_completion(with_client(&sched,
                                        server.local_port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const std::array<hpack::field, 3> trailers{
                                                hpack::field{.name = "x-checksum",   .value = "abc"},
                                                hpack::field{  .name = "if-match", .value = "\"x\""},
                                                hpack::field{.name = "x-checksum",   .value = "def"},
                                            };

                                            // the trailers come after the body, but before the handler reads it: it
                                            // still finds them only once it's read it to its end.
                                            const auto id = c.request("POST", "/echo", "hello", false);
                                            c.send_trailers(id, trailers);
                                            const bool done = co_await c.wait_for(id);
                                            REQUIRE(done);

                                            const auto& r = c.responses[id];
                                            CAPTURE(r.reset.message());
                                            CHECK(r.status == "200");
                                            CHECK(r.body == "hello|x-checksum=abc,def;");
                                            CHECK(r.trailers
                                                  == std::vector<std::pair<std::string, std::string>>{
                                                      {"x-checksum", "abc"},
                                                      {"x-checksum", "def"},
                                            });

                                            // and a request whose trailers come straight after its head.
                                            const auto bare = c.request("POST", "/echo", {}, false);
                                            c.send_trailers(bare, std::span{trailers}.first(1));
                                            const bool done_bare = co_await c.wait_for(bare);
                                            REQUIRE(done_bare);
                                            CHECK(c.responses[bare].body == "|x-checksum=abc;");
                                        }));
}

TEST_CASE("an HTTP/2 response's trailers end its stream, whatever its body", "[http][http2][server][trailers]")
{
    net::io::scheduler sched{make_pool()};
    http::server       server{
        &sched,
        make_router(&sched),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    sched.run_to_completion(with_client(&sched,
                                        server.local_port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const auto chunked  = c.request("GET", "/chunked");
                                            const auto length   = c.request("GET", "/length");
                                            const auto empty    = c.request("GET", "/empty");
                                            const auto declared = c.request("GET", "/declared");
                                            const auto head     = c.request("HEAD", "/chunked");
                                            const auto invalid  = c.request("GET", "/invalid");
                                            const auto thrown   = c.request("GET", "/throw");
                                            const bool finished = co_await c.pump([&] { return c.finished == 7; });
                                            REQUIRE(finished);

                                            const std::vector<std::pair<std::string, std::string>> expected{
                                                {"server-timing", "db;dur=53"},
                                                {  "grpc-status",         "0"},
                                            };

                                            // after a body of unknown length, without those that can't be trailers.
                                            const auto& r1 = c.responses[chunked];
                                            CHECK(r1.status == "200");
                                            CHECK(r1.body == "hello");
                                            CHECK(r1.trailers == expected);

                                            // after a body with a Content-Length.
                                            const auto& r2 = c.responses[length];
                                            CHECK(header(r2, "content-length") == "5");
                                            CHECK(r2.body == "hello");
                                            CHECK(r2.trailers == expected);

                                            // straight after a head without a body.
                                            const auto& r3 = c.responses[empty];
                                            CHECK(r3.status == "200");
                                            CHECK_FALSE(r3.head_ended);
                                            CHECK(r3.data_frames == 0);
                                            CHECK(r3.trailers == expected);

                                            // declared, but none set: an empty DATA frame ends the stream.
                                            const auto& r4 = c.responses[declared];
                                            CHECK(header(r4, "trailer") == "Grpc-Status");
                                            CHECK_FALSE(r4.head_ended);
                                            CHECK(r4.data_frames == 1);
                                            CHECK(r4.trailers.empty());

                                            // none for HEAD, whose head ends the stream.
                                            const auto& r5 = c.responses[head];
                                            CHECK(r5.head_ended);
                                            CHECK(r5.trailers.empty());

                                            // an invalid one: the stream's reset, not ended.
                                            const auto& r6 = c.responses[invalid];
                                            CHECK(r6.reset == http2::error_code::internal_error);
                                            CHECK(r6.trailers.empty());

                                            // the 500 for a handler that throws has none.
                                            const auto& r7 = c.responses[thrown];
                                            CHECK(r7.status == "500");
                                            CHECK(r7.head_ended);
                                            CHECK(r7.trailers.empty());
                                        }));
}

TEST_CASE("middleware adds an HTTP/2 response's trailers after the handler", "[http][http2][server][trailers]")
{
    net::io::scheduler sched{make_pool()};
    http::server       server{
        &sched,
        http::chain(served_by{},
        make_router(&sched)),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    sched.run_to_completion(with_client(&sched,
                                        server.local_port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const auto id   = c.request("GET", "/length");
                                            const bool done = co_await c.wait_for(id);
                                            REQUIRE(done);

                                            const auto& r = c.responses[id];
                                            CHECK(trailer(r, "server-timing") == "db;dur=53");
                                            CHECK(trailer(r, "x-served-by") == "middleware");
                                        }));
}
