// The server's buffer and window sizes (server_config's read_buffer_size, write_buffer_size, http2_read_buffer_size and
// http2_stream_window): what the buffers carry goes through whatever their sizes (heads longer than the read buffer,
// pipelined requests, responses longer than the write buffer, HTTP/2's frames read a buffer at a time), the stream
// window reaches the client in SETTINGS and the connection's window follows it, and sizes out of their ranges are
// refused when the server's made.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/h2_client.hpp"
#include "http/http.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "url.hpp"

namespace http = net::http;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// make_router answers POST /echo with the request's body, and an x-back field of its x-long field's value.
http::router make_router()
{
    http::router r;
    r.POST("/echo",
           [](http::server_request& req, http::response_writer& resp) -> task<void>
           {
               auto body = co_await net::io::read_all(*req.body);
               if (!body.has_value())
               {
                   co_await resp.send(http::status::BAD_REQUEST, 0);
                   co_return;
               }

               resp.headers().set("x-back"sv, req.headers.get("x-long"sv).value_or(""sv));
               auto* out = co_await resp.send(http::status::OK, body->size());
               (void)co_await net::io::write_all(*out, *body);
           });
    return r;
}

// exchange sends request to the server on port as it is, and reads until the server closes the connection.
task<std::string> exchange(net::io::scheduler* sched, std::uint16_t port, std::string request)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    if (!conn.has_value()) co_return "error: " + conn.error().message();
    conn->set_read_timeout(5s);

    auto res = co_await net::io::write_all(*conn, request);
    if (res.err) co_return "error: " + res.err.message();

    std::string                  out;
    std::array<std::byte, 4'096> buf{};
    while (true)
    {
        auto read = co_await conn->read(buf);
        out.append(reinterpret_cast<const char*>(buf.data()), read.count);
        if (read.err) break;
    }
    co_return out;
}

// count counts the occurrences of what in str.
std::size_t count(std::string_view str, std::string_view what)
{
    std::size_t n = 0;
    for (auto pos = str.find(what); pos != std::string_view::npos; pos = str.find(what, pos + what.size())) ++n;
    return n;
}

// echoed POSTs body to /echo over HTTP/2 (prior knowledge), returning the response's body, or the error.
task<std::string> echoed(net::io::scheduler* sched, std::uint16_t port, std::string body)
{
    http::client_options options;
    options.http2_prior_knowledge = true;
    http::client client{sched, options};

    http::client_request req{.method = http::request_method::POST,
                             .uri    = net::url::parse("http://127.0.0.1:" + std::to_string(port) + "/echo").value()};
    req.set_body(std::move(body));

    auto resp = co_await client.send(std::move(req));
    if (!resp.has_value()) co_return "error: " + resp.error().message();
    if (resp->version.major != 2) co_return "error: not HTTP/2";

    auto read = co_await net::io::read_all(*resp->body);
    if (!read.has_value()) co_return "error: " + read.error().message();
    co_return std::move(*read);
}

}

TEST_CASE("HTTP/1.1 heads and bodies longer than the server's buffers go through them", "[http][server][buffers]")
{
    struct test_case
    {
        std::string_view name;
        std::size_t      read  = 0;
        std::size_t      write = 0;
    };

    const auto tc = GENERATE(test_case{.name = "the least", .read = 1'024, .write = 1'024},
                             test_case{.name = "the defaults", .read = 16'384, .write = 16'384},
                             test_case{.name = "large", .read = 65'536, .write = 65'536});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    http::server       server{
        &sched,
        net::listener::make(&sched, "127.0.0.1", "0").value(),
        make_router(),
        {.read_buffer_size = tc.read, .write_buffer_size = tc.write}
    };
    REQUIRE(sched.spawn(server.serve()));

    // three requests pipelined in one write, each with a field longer than the least buffer (and a body), so a head
    // line is read apart where it's longer than the read buffer, and each response's head and body are longer than
    // the least write buffer.
    const std::string long_value(3'000, 'x');
    std::string       requests;
    for (char c : {'a', 'b', 'c'})
    {
        const auto close = c == 'c' ? "Connection: close\r\n"sv : ""sv;
        requests += "POST /echo HTTP/1.1\r\nHost: localhost\r\nx-long: " + long_value + "\r\n" + std::string{close}
                  + "Content-Length: 5000\r\n\r\n" + std::string(5'000, c);
    }

    const auto out = sched.run_to_completion(exchange(&sched, server.local_port(), requests));
    CHECK(count(out, "HTTP/1.1 200 OK\r\n") == 3);
    CHECK(count(out, "x-back: " + long_value + "\r\n") == 3);
    for (char c : {'a', 'b', 'c'}) CHECK(count(out, std::string(5'000, c)) == 1);
}

TEST_CASE("HTTP/2 frames longer than the server's read buffer are read a buffer at a time",
          "[http][http2][server][buffers]")
{
    const auto size = GENERATE(std::size_t{1'024}, std::size_t{65'536});
    CAPTURE(size);

    net::io::scheduler sched{make_pool()};
    http::server       server{&sched,
                              net::listener::make(&sched, "127.0.0.1", "0").value(),
                              make_router(),
                              {.http2_read_buffer_size = size}};
    REQUIRE(sched.spawn(server.serve()));

    std::string body(200'000, '\0');
    for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>('a' + (i % 26));

    const auto out = sched.run_to_completion(echoed(&sched, server.local_port(), body));
    CHECK(out.size() == body.size());
    CHECK(out == body);
}

TEST_CASE("the server's HTTP/2 stream window is its SETTINGS', and its connection's window holds its streams'",
          "[http][http2][server][buffers][window]")
{
    struct test_case
    {
        std::string_view name;
        std::uint32_t    window  = 0;
        std::uint32_t    streams = 100;
        std::int64_t     want    = 0; // the connection's window
    };

    const auto tc =
        GENERATE(test_case{.name = "the default", .window = 65'535, .want = std::int64_t{100} * 65'535},
                 test_case{.name = "at least 1 MiB", .window = 65'535, .streams = 10, .want = std::int64_t{1} << 20},
                 test_case{.name = "1 MiB", .window = 1U << 20, .want = std::int64_t{100} << 20},
                 test_case{.name = "the most a window may be", .window = 0x7fff'ffff, .want = 0x7fff'ffff},
                 test_case{.name = "the most a connection's may be", .window = 1U << 30, .want = 0x7fff'ffff});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    http::server       server{
        &sched,
        net::listener::make(&sched, "127.0.0.1", "0").value(),
        make_router(),
        {.http2_max_concurrent_streams = tc.streams, .http2_stream_window = tc.window}
    };
    REQUIRE(sched.spawn(server.serve()));

    // what the client saw: the stream window in the server's SETTINGS, the connection's window, and a stream's.
    struct seen
    {
        std::string   failed; // what went wrong with the test's own steps, if anything
        std::uint32_t initial_window    = 0;
        std::int64_t  connection_window = 0;
        std::int64_t  stream_window     = 0;
    };

    const auto saw = sched.run_to_completion(
        [](net::io::scheduler* sched, std::uint16_t port) -> task<seen>
        {
            seen out;

            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
            if (!conn.has_value())
            {
                out.failed = "not connected: " + conn.error().message();
                co_return out;
            }
            test::h2_client c{&*conn, &*conn, &*conn};

            // the server's SETTINGS, and the WINDOW_UPDATE that raises its connection's window after them.
            const bool synced = co_await c.sync();
            if (!synced)
            {
                out.failed = "no PING ACK";
                co_return out;
            }
            out.initial_window    = c.s.peer_settings().initial_window_size;
            out.connection_window = c.s.send_window(0);

            // a stream may send as much as its window, which the connection's holds.
            const auto id     = c.request("POST", "/echo", {}, false);
            out.stream_window = c.s.send_window(id);
            out.failed        = c.failure; // (the request's head refused, if it was)
            co_return out;
        }(&sched, server.local_port()));

    CAPTURE(saw.failed);
    CHECK(saw.failed.empty());
    CHECK(saw.initial_window == tc.window);
    CHECK(saw.connection_window == tc.want);
    CHECK(std::cmp_equal(saw.stream_window, tc.window));
}

TEST_CASE("the server refuses buffer and window sizes out of their ranges", "[http][server][buffers][limits]")
{
    struct test_case
    {
        std::string_view    name;
        http::server_config cfg;
        bool                refused = true;
    };

    const auto tc = GENERATE(
        test_case{
            .name = "read_buffer_size under 1 KiB",
            .cfg  = {.read_buffer_size = 1'023}
    },
        test_case{.name = "write_buffer_size under 1 KiB", .cfg = {.write_buffer_size = 1'023}},
        test_case{.name = "http2_read_buffer_size under 1 KiB", .cfg = {.http2_read_buffer_size = 1'023}},
        test_case{.name = "http2_stream_window under 65535", .cfg = {.http2_stream_window = 65'534}},
        test_case{.name = "http2_stream_window over 2^31-1", .cfg = {.http2_stream_window = 0x8000'0000}},
        test_case{.name    = "the least of each",
                  .cfg     = {.read_buffer_size       = 1'024,
                              .write_buffer_size      = 1'024,
                              .http2_read_buffer_size = 1'024,
                              .http2_stream_window    = 65'535},
                  .refused = false},
        test_case{.name = "the most window", .cfg = {.http2_stream_window = 0x7fff'ffff}, .refused = false});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    const auto         make = [&]
    { return http::server{&sched, net::listener::make(&sched, "127.0.0.1", "0").value(), make_router(), tc.cfg}; };

    if (tc.refused) CHECK_THROWS_AS(make(), std::invalid_argument);
    else CHECK_NOTHROW(make());
}
