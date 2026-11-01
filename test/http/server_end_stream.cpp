// How the server's HTTP/2 responses end: one with a Content-Length ends with its last DATA frame (END_STREAM on it),
// rather than with an empty DATA frame after it, whether its writes go at once or wait for the connection's writer; one
// without a body with its head; and one without a Content-Length with an empty DATA frame (or its trailers) once the
// handler has returned.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/h2_client.hpp"
#include "http/hpack.hpp"
#include "http/http2.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;
namespace hpack = net::http::hpack;

using namespace std::string_view_literals;
using net::coro::task;
using test::h2_client;
using test::header;

namespace
{

// number is a request's field's value as a number, or otherwise if it hasn't the field.
std::size_t number(const http::server_request& req, std::string_view name, std::size_t otherwise)
{
    const auto value = req.headers.get(name);
    return value.has_value() ? std::stoul(std::string{*value}) : otherwise;
}

// make_router answers /bytes with x-size bytes, written in x-writes writes (the last taking what's left), with a
// Content-Length of x-length (x-size, if it's not given), or without one if the request has x-unknown-length; then
// throws, if it has x-throw.
http::router make_router()
{
    http::router r;
    r.GET("/bytes",
          [](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              const auto size   = number(req, "x-size"sv, 0);
              const auto length = number(req, "x-length"sv, size);
              const auto writes = std::max<std::size_t>(number(req, "x-writes"sv, 1), 1);

              net::io::writer* body = nullptr;
              if (req.headers.get("x-unknown-length"sv).has_value()) body = co_await resp.send(http::status::OK);
              else body = co_await resp.send(http::status::OK, length);

              const std::string data(size, 'x');
              const auto        bytes   = std::as_bytes(std::span{data});
              std::size_t       written = 0;
              for (std::size_t i = 0; i < writes; ++i)
              {
                  const auto n   = i + 1 == writes ? size - written : size / writes;
                  const auto res = co_await body->write(bytes.subspan(written, n));
                  if (res.err) co_return;
                  written += n;
              }

              if (req.headers.get("x-throw"sv).has_value()) throw std::runtime_error{"after the body"};
          });
    return r;
}

struct test_server
{
    explicit test_server(net::io::scheduler* sched)
        : server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), make_router()}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    [[nodiscard]] std::string port() const { return std::to_string(server.local_port()); }

    http::server server;
};

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// seen is what an h2_client received: its responses, by stream, and its non-empty DATA frames, in order.
struct seen
{
    std::map<http2::stream_id, h2_client::response>       responses;
    std::vector<std::pair<http2::stream_id, std::size_t>> data_log;
};

// with_client connects to port, and runs test with an h2_client (its session's config) over the connection, keeping
// what it received in got. It returns what went wrong, if anything: test returns what went wrong with its own steps.
template<typename Test>
task<std::string>
with_client(net::io::scheduler* sched, std::string port, seen* got, Test test, http2::session_config config = {})
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
    if (!conn.has_value()) co_return "not connected: " + conn.error().message();

    h2_client client{&*conn, &*conn, &*conn, http2::role::client, config};
    auto      failed = co_await test(client);
    if (failed.empty()) failed = client.failure;
    got->responses = client.responses;
    got->data_log  = client.data_log;
    co_return failed;
}

// bytes is a request for /bytes.
struct bytes
{
    std::size_t                size           = 0;
    std::size_t                writes         = 1;
    std::optional<std::size_t> length         = std::nullopt; // the Content-Length, if not size
    bool                       unknown_length = false;
    bool                       throws         = false;
    std::string_view           method         = "GET";
};

// request_bytes sends a request for /bytes.
http2::stream_id request_bytes(h2_client& c, const bytes& b)
{
    const auto                size   = std::to_string(b.size);
    const auto                writes = std::to_string(b.writes);
    const auto                length = std::to_string(b.length.value_or(b.size));
    std::vector<hpack::field> fields{
        {  .name = "x-size",   .value = size},
        {.name = "x-writes", .value = writes},
        {.name = "x-length", .value = length},
    };
    if (b.unknown_length) fields.push_back({.name = "x-unknown-length", .value = "1"});
    if (b.throws) fields.push_back({.name = "x-throw", .value = "1"});
    return c.request(b.method, "/bytes", {}, true, fields);
}

// data_frames is how many non-empty DATA frames the client received on a stream.
std::size_t data_frames(const seen& got, http2::stream_id id)
{
    return static_cast<std::size_t>(std::ranges::count_if(got.data_log, [id](const auto& f) { return f.first == id; }));
}

constexpr std::size_t frame = 16'384; // the client's MAX_FRAME_SIZE: the server's DATA frames' largest

}

TEST_CASE("an HTTP/2 response with a Content-Length ends with its last DATA frame", "[http][http2][server][end-stream]")
{
    struct test_case
    {
        std::string_view name;
        bytes            request;
        int              frames; // DATA frames: none empty
        std::size_t      last;   // the last's length, which ends the stream
    };

    const auto tc = GENERATE(
        test_case{
            .name    = "a write",
            .request = {.size = 5},
            .frames  = 1,
            .last    = 5
    },
        test_case{.name = "several writes", .request = {.size = 5, .writes = 3}, .frames = 3, .last = 3},
        test_case{.name    = "a write cut into frames",
                  .request = {.size = 40'000},
                  .frames  = 3,
                  .last    = 40'000 - (2 * frame)},
        test_case{.name = "a write of whole frames", .request = {.size = 2 * frame}, .frames = 2, .last = frame},
        test_case{.name    = "writes of a frame and more",
                  .request = {.size = 40'000, .writes = 4},
                  .frames  = 4,
                  .last    = 10'000},
        test_case{.name    = "a handler that throws once it's written it all",
                  .request = {.size = 5, .throws = true},
                  .frames  = 1,
                  .last    = 5});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    // (a connection of its own, with windows for all of it: its writes go at once, not through the writer's turns.)
    seen             got;
    http2::stream_id id     = 0;
    const auto       failed = sched.run_to_completion(with_client(&sched,
                                                                  server.port(),
                                                                  &got,
                                                                  [&](h2_client& c) -> task<std::string>
                                                                  {
                                                                id              = request_bytes(c, tc.request);
                                                                const bool done = co_await c.wait_for(id);
                                                                co_return done ? "" : "not done";
                                                                  }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    const auto& r = got.responses[id];
    CAPTURE(r.reset.message());
    CHECK_FALSE(r.reset);
    CHECK(r.status == "200");
    CHECK(header(r, "content-length") == std::to_string(tc.request.size));
    CHECK(r.body.size() == tc.request.size);
    CHECK_FALSE(r.head_ended);
    CHECK(r.data_frames == tc.frames);
    CHECK(data_frames(got, id) == static_cast<std::size_t>(tc.frames));
    CHECK(r.ended_by_data == tc.last);
}

TEST_CASE("an HTTP/2 response with a Content-Length that waits for its window ends with its last DATA frame",
          "[http][http2][server][end-stream]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    // a stream's window of 20000: the first 20000 bytes go at once, and the rest waits for the writer to frame it as
    // the client gives the window back.
    http2::session_config config;
    config.local.initial_window_size = 20'000;

    constexpr std::size_t size = 50'000;
    seen                  got;
    http2::stream_id      id     = 0;
    const auto            failed = sched.run_to_completion(with_client(
        &sched,
        server.port(),
        &got,
        [&](h2_client& c) -> task<std::string>
        {
            id              = request_bytes(c, {.size = size});
            const bool done = co_await c.wait_for(id);
            co_return done ? "" : "not done";
        },
        config));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    const auto& r = got.responses[id];
    CAPTURE(r.reset.message());
    CHECK_FALSE(r.reset);
    CHECK(r.body.size() == size);

    // whatever the frames' sizes, none empty, the last ending the stream.
    CHECK(r.data_frames >= 4);
    CHECK(data_frames(got, id) == static_cast<std::size_t>(r.data_frames));
    REQUIRE(r.ended_by_data.has_value());
    CHECK(*r.ended_by_data > 0);
    REQUIRE_FALSE(got.data_log.empty());
    CHECK(*r.ended_by_data == got.data_log.back().second);
}

TEST_CASE("HTTP/2 responses without a body, to HEAD, and without a Content-Length end as before",
          "[http][http2][server][end-stream]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    seen             got;
    http2::stream_id empty   = 0;
    http2::stream_id head    = 0;
    http2::stream_id unknown = 0;
    http2::stream_id nothing = 0;
    const auto       failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            &got,
                                            [&](h2_client& c) -> task<std::string>
                                            {
                                                empty   = request_bytes(c, {.size = 0});
                                                head    = request_bytes(c, {.size = 5, .method = "HEAD"});
                                                unknown = request_bytes(c, {.size = 5, .unknown_length = true});
                                                nothing = request_bytes(c, {.size = 0, .unknown_length = true});
                                                const bool finished = co_await c.pump([&] { return c.finished == 4; });
                                                co_return finished ? "" : "not all of them ended";
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    // its head ends a response without a body...
    const auto& r1 = got.responses[empty];
    CHECK(header(r1, "content-length") == "0");
    CHECK(r1.head_ended);
    CHECK(r1.data_frames == 0);

    // ...and one to HEAD, whatever its Content-Length.
    const auto& r2 = got.responses[head];
    CHECK(header(r2, "content-length") == "5");
    CHECK(r2.head_ended);
    CHECK(r2.data_frames == 0);
    CHECK(r2.body.empty());

    // without a Content-Length, an empty DATA frame once the handler's returned: the server can't know which write is
    // the last.
    const auto& r3 = got.responses[unknown];
    CHECK(header(r3, "content-length") == "(none)");
    CHECK(r3.body == "xxxxx");
    CHECK_FALSE(r3.head_ended);
    CHECK(r3.data_frames == 2);
    CHECK(r3.ended_by_data == 0U);

    const auto& r4 = got.responses[nothing];
    CHECK_FALSE(r4.head_ended);
    CHECK(r4.data_frames == 1);
    CHECK(r4.ended_by_data == 0U);

    for (const auto id : {empty, head, unknown, nothing})
    {
        CAPTURE(id);
        CHECK_FALSE(got.responses[id].reset);
        CHECK(got.responses[id].status == "200");
    }
}

TEST_CASE("an HTTP/2 handler that writes less than its Content-Length has its stream reset",
          "[http][http2][server][end-stream]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    seen             got;
    http2::stream_id id     = 0;
    const auto       failed = sched.run_to_completion(with_client(&sched,
                                                                  server.port(),
                                                                  &got,
                                                                  [&](h2_client& c) -> task<std::string>
                                                                  {
                                                                id = request_bytes(c, {.size = 4, .length = 10});
                                                                const bool done = co_await c.wait_for(id);
                                                                co_return done ? "" : "not done";
                                                                  }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    // what was written went, but never ended the stream.
    const auto& r = got.responses[id];
    CHECK(r.body == "xxxx");
    CHECK(r.reset == http2::error_code::internal_error);
    CHECK_FALSE(r.ended_by_data.has_value());
}
