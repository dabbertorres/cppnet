// The client's buffer and window sizes (client_options' read_buffer_size, write_buffer_size, http2_read_buffer_size and
// http2_stream_window): a response's head and body go through the read buffer whatever its size (lines longer than it,
// a body read in pieces smaller than it); a request's body is read write_buffer_size at a time, which the server sees
// as its chunks (HTTP/1.1) or DATA frames (HTTP/2); HTTP/2's frames are read a buffer at a time; the stream window
// reaches the server in SETTINGS; and sizes out of their ranges are refused when the client's made.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
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
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/h2_client.hpp"
#include "http/http.hpp"
#include "http/http2.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/string_reader.hpp"
#include "io/util.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "url.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// read_until reads from conn into got until it ends with end (within 10 seconds), returning whether it does.
task<bool> read_until(net::tcp_socket& conn, std::string& got, std::string_view end)
{
    conn.set_read_deadline(net::io::clock::now() + 10s);

    std::array<std::byte, 4'096> buf{};
    while (!got.ends_with(end))
    {
        auto res = co_await conn.read(buf);
        got.append(reinterpret_cast<const char*>(buf.data()), res.count);
        if (res.err) co_return got.ends_with(end);
    }
    co_return true;
}

// chunk_sizes is the sizes of a chunked body's chunks (but the last, empty one), body being what follows the head.
std::vector<std::size_t> chunk_sizes(std::string_view body)
{
    std::vector<std::size_t> sizes;
    while (true)
    {
        const auto line = body.find("\r\n"sv);
        const auto size = std::stoul(std::string{body.substr(0, line)}, nullptr, 16);
        if (size == 0) return sizes;

        sizes.push_back(size);
        body.remove_prefix(line + 2 + size + 2);
    }
}

// outcome is what a request sent in the background came to: its response's status and body, or the error.
struct outcome
{
    std::atomic<std::uint64_t> finished{0};
    int                        status = 0;
    std::string                cookie; // its Set-Cookie
    std::string                body;
    std::error_code            err;

    // wait waits (up to 10 seconds) for the request to have finished, returning whether it has.
    task<bool> wait(net::io::scheduler* sched)
    {
        const auto deadline = net::io::clock::now() + 10s;
        while (finished.load() == 0)
        {
            if (co_await sched->wait_on(finished, 0, deadline)) break;
        }
        co_return finished.load() != 0;
    }
};

// send_with sends req with client, recording what it came to in out (reading the response's body in pieces of 100
// bytes).
task<> send_with(net::io::scheduler* sched, http::client* client, http::client_request req, outcome* out)
{
    auto resp = co_await client->send(std::move(req));
    if (!resp.has_value())
    {
        out->err = resp.error();
    }
    else
    {
        out->status = static_cast<int>(resp->status_code);
        out->cookie = std::string{resp->headers.get("Set-Cookie"sv).value_or(""sv)};

        std::array<std::byte, 100> piece{};
        while (true)
        {
            auto res = co_await resp->body->read(piece);
            out->body.append(reinterpret_cast<const char*>(piece.data()), res.count);
            if (res.err != net::io::errc::end_of_stream) out->err = res.err;
            if (res.err) break;
        }
    }

    out->finished.store(1);
    sched->notify_all(out->finished);
}

// upload is a POST to url of body: with its Content-Length if known, or else chunked.
http::client_request upload(const std::string& url, std::string body, bool known)
{
    http::client_request req{.method = http::request_method::POST, .uri = net::url::parse(url).value()};
    if (known) req.set_body(std::move(body));
    else req.body = std::make_unique<net::io::owned_string_reader>(std::move(body));
    return req;
}

// make_router answers POST /echo with the request's body.
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
               auto* out = co_await resp.send(http::status::OK, body->size());
               (void)co_await net::io::write_all(*out, *body);
           });
    return r;
}

// letters is size bytes of the alphabet, over and over.
std::string letters(std::size_t size)
{
    std::string out(size, '\0');
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<char>('a' + (i % 26));
    return out;
}

}

TEST_CASE("an HTTP/1.1 response goes through the client's read buffer whatever its size", "[http][client][buffers]")
{
    const auto size = GENERATE(std::size_t{1'024}, std::size_t{4'096}, std::size_t{65'536});
    CAPTURE(size);

    net::io::scheduler sched{make_pool()};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    http::client_options options;
    options.read_buffer_size = size;
    http::client client{&sched, options};

    // a head line longer than the least buffer, and a chunked body of chunks larger and smaller than it, which the
    // caller reads in pieces smaller than it.
    const std::string cookie(5'000, 'c');
    const auto        body = letters(100'000);

    outcome out;
    sched.run_to_completion(
        [](net::io::scheduler* sched,
           net::listener*      listener,
           http::client*       client,
           const std::string*  cookie,
           const std::string*  body,
           outcome*            out) -> task<>
        {
            auto url = std::format("http://127.0.0.1:{}/", listener->local_port());
            REQUIRE(
                sched->spawn(send_with(sched, client, http::client_request{.uri = net::url::parse(url).value()}, out)));

            auto accepted = co_await listener->accept();
            REQUIRE(accepted.has_value());

            std::string request;
            const bool  read = co_await read_until(*accepted, request, "\r\n\r\n");
            REQUIRE(read);

            std::string response =
                "HTTP/1.1 200 OK\r\nSet-Cookie: " + *cookie + "\r\nTransfer-Encoding: chunked\r\n\r\n";
            for (std::size_t at = 0, n = 1; at < body->size(); at += n, n = (n * 7) + 3)
            {
                n = std::min(n, body->size() - at);
                response += std::format("{:x}\r\n{}\r\n", n, std::string_view{*body}.substr(at, n));
            }
            response += "0\r\n\r\n";
            const auto written = co_await net::io::write_all(*accepted, response);
            REQUIRE_FALSE(written.err);

            const bool finished = co_await out->wait(sched);
            REQUIRE(finished);
        }(&sched, &listener, &client, &cookie, &body, &out));

    CHECK_FALSE(out.err);
    CHECK(out.status == 200);
    CHECK(out.cookie == cookie);
    CHECK(out.body.size() == body.size());
    CHECK(out.body == body);
}

TEST_CASE("an HTTP/1.1 request's body is read write_buffer_size at a time", "[http][client][buffers]")
{
    const auto size = GENERATE(std::size_t{1'024}, std::size_t{4'096}, std::size_t{16'384}, std::size_t{65'536});
    CAPTURE(size);

    net::io::scheduler sched{make_pool()};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    http::client_options options;
    options.write_buffer_size = size;
    http::client client{&sched, options};

    // chunked, so each read of the body is a chunk.
    constexpr std::size_t length = 50'000;
    const auto            sizes  = sched.run_to_completion(
        [](net::io::scheduler* sched, net::listener* listener, http::client* client) -> task<std::vector<std::size_t>>
        {
            outcome out;
            auto    url = std::format("http://127.0.0.1:{}/", listener->local_port());
            REQUIRE(sched->spawn(send_with(sched, client, upload(url, std::string(length, 'x'), false), &out)));

            auto accepted = co_await listener->accept();
            REQUIRE(accepted.has_value());

            std::string request;
            const bool  read = co_await read_until(*accepted, request, "\r\n0\r\n\r\n");
            REQUIRE(read);

            const auto written =
                co_await net::io::write_all(*accepted, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"sv);
            REQUIRE_FALSE(written.err);
            const bool finished = co_await out.wait(sched);
            REQUIRE(finished);
            CHECK(out.status == 200);

            co_return chunk_sizes(std::string_view{request}.substr(request.find("\r\n\r\n") + 4));
        }(&sched, &listener, &client));

    std::vector<std::size_t> want(length / std::min(size, length), std::min(size, length));
    if (length % size != 0 && size < length) want.push_back(length % size);
    CHECK(sizes == want);
}

TEST_CASE("an HTTP/1.1 request's body of known length goes whole through a small write buffer",
          "[http][client][buffers]")
{
    net::io::scheduler sched{make_pool()};
    http::server       server{&sched, net::listener::make(&sched, "127.0.0.1", "0").value(), make_router()};
    REQUIRE(sched.spawn(server.serve()));

    http::client_options options;
    options.write_buffer_size = 1'024;
    http::client client{&sched, options};

    const auto body = letters(100'000);
    outcome    out;
    sched.run_to_completion(send_with(&sched,
                                      &client,
                                      upload(std::format("http://127.0.0.1:{}/echo", server.local_port()), body, true),
                                      &out));
    CHECK_FALSE(out.err);
    CHECK(out.status == 200);
    CHECK(out.body == body);
}

TEST_CASE("an HTTP/2 request's body is read write_buffer_size at a time", "[http][http2][client][buffers]")
{
    const auto size = GENERATE(std::size_t{1'024}, std::size_t{4'096}, std::size_t{16'384}, std::size_t{65'536});
    CAPTURE(size);

    net::io::scheduler sched{make_pool()};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    http::client_options options;
    options.http2_prior_knowledge = true;
    options.write_buffer_size     = size;
    http::client client{&sched, options};

    // within the server's windows (65535 each), so each read of the body is written at once, as its DATA frames (of at
    // most 16 KiB, the server's MAX_FRAME_SIZE).
    constexpr std::size_t length = 50'000;
    const auto            frames = sched.run_to_completion(
        [](net::io::scheduler* sched, net::listener* listener, http::client* client) -> task<std::vector<std::size_t>>
        {
            outcome out;
            auto    url = std::format("http://127.0.0.1:{}/", listener->local_port());
            REQUIRE(sched->spawn(send_with(sched, client, upload(url, std::string(length, 'x'), true), &out)));

            auto accepted = co_await listener->accept();
            REQUIRE(accepted.has_value());
            test::h2_client server{&*accepted, &*accepted, &*accepted, http2::role::server};

            bool ok = co_await server.pump([&] { return server.responses[1].done; });
            REQUIRE(ok);
            server.respond(1, "200");
            ok = co_await server.pump([] { return true; });
            REQUIRE(ok);
            const bool finished = co_await out.wait(sched);
            REQUIRE(finished);
            CHECK(out.status == 200);
            CHECK(server.responses[1].body == std::string(length, 'x'));

            std::vector<std::size_t> sizes;
            sizes.reserve(server.data_log.size());
            for (const auto& [id, n] : server.data_log) sizes.push_back(n);
            co_return sizes;
        }(&sched, &listener, &client));

    const auto               each = std::min(size, std::size_t{16'384});
    std::vector<std::size_t> want(length / each, each);
    want.push_back(length % each);
    CHECK(frames == want);
}

TEST_CASE("HTTP/2 frames longer than the client's read buffer are read a buffer at a time",
          "[http][http2][client][buffers]")
{
    const auto size = GENERATE(std::size_t{1'024}, std::size_t{65'536});
    CAPTURE(size);

    net::io::scheduler sched{make_pool()};
    http::server       server{&sched, net::listener::make(&sched, "127.0.0.1", "0").value(), make_router()};
    REQUIRE(sched.spawn(server.serve()));

    http::client_options options;
    options.http2_prior_knowledge  = true;
    options.http2_read_buffer_size = size;
    http::client client{&sched, options};

    const auto body = letters(200'000);
    outcome    out;
    sched.run_to_completion(send_with(&sched,
                                      &client,
                                      upload(std::format("http://127.0.0.1:{}/echo", server.local_port()), body, true),
                                      &out));
    CHECK_FALSE(out.err);
    CHECK(out.status == 200);
    CHECK(out.body == body);
}

TEST_CASE("the client's HTTP/2 stream window is its SETTINGS'", "[http][http2][client][buffers][window]")
{
    const auto window = GENERATE(std::uint32_t{65'535}, std::uint32_t{1} << 20, std::uint32_t{0x7fff'ffff});
    CAPTURE(window);

    net::io::scheduler sched{make_pool()};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    http::client_options options;
    options.http2_prior_knowledge = true;
    options.http2_stream_window   = window;
    http::client client{&sched, options};

    sched.run_to_completion(
        [](net::io::scheduler* sched, net::listener* listener, http::client* client, std::uint32_t window) -> task<>
        {
            outcome out;
            auto    url = std::format("http://127.0.0.1:{}/", listener->local_port());
            REQUIRE(sched->spawn(
                send_with(sched, client, http::client_request{.uri = net::url::parse(url).value()}, &out)));

            auto accepted = co_await listener->accept();
            REQUIRE(accepted.has_value());
            test::h2_client server{&*accepted, &*accepted, &*accepted, http2::role::server};

            bool ok = co_await server.pump([&] { return server.responses[1].done; });
            REQUIRE(ok);
            CHECK(server.s.peer_settings().initial_window_size == window);

            // the response may send as much as the stream's window, or the connection's (1 GiB), if that's less.
            CHECK(server.s.send_window(1) == std::min(window, std::uint32_t{1} << 30));

            server.respond(1, "200");
            ok = co_await server.pump([] { return true; });
            REQUIRE(ok);
            const bool finished = co_await out.wait(sched);
            REQUIRE(finished);
            CHECK(out.status == 200);
        }(&sched, &listener, &client, window));
}

TEST_CASE("the client refuses buffer and window sizes out of their ranges", "[http][client][buffers][limits]")
{
    struct test_case
    {
        std::string_view     name;
        http::client_options options;
        bool                 refused = true;
    };

    const auto tc = GENERATE(
        test_case{
            .name    = "read_buffer_size under 1 KiB",
            .options = {.read_buffer_size = 1'023}
    },
        test_case{.name = "write_buffer_size under 1 KiB", .options = {.write_buffer_size = 1'023}},
        test_case{.name = "http2_read_buffer_size under 1 KiB", .options = {.http2_read_buffer_size = 1'023}},
        test_case{.name = "http2_stream_window under 65535", .options = {.http2_stream_window = 65'534}},
        test_case{.name = "http2_stream_window over 2^31-1", .options = {.http2_stream_window = 0x8000'0000}},
        test_case{.name    = "the least of each",
                  .options = {.read_buffer_size       = 1'024,
                              .write_buffer_size      = 1'024,
                              .http2_read_buffer_size = 1'024,
                              .http2_stream_window    = 65'535},
                  .refused = false},
        test_case{.name = "the most window", .options = {.http2_stream_window = 0x7fff'ffff}, .refused = false});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    if (tc.refused) CHECK_THROWS_AS(http::client(&sched, tc.options), std::invalid_argument);
    else CHECK_NOTHROW(http::client(&sched, tc.options));
}
