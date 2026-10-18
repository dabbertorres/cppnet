#include "http/server.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception> // IWYU pragma: keep
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h> // IWYU pragma: keep (timeval, on Linux)
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/callback_sink.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "tcp.hpp"
#include "url.hpp"
#include "util/defer.hpp"

namespace http = net::http;

namespace
{

using namespace std::chrono_literals;
using namespace std::string_view_literals;

using net::coro::task;

struct response
{
    http::status status = http::status::NONE;
    std::string  connection;
    std::string  body;
};

// get requests path from the server on port, and reads the whole response.
task<response> get(net::io::scheduler* sched, std::uint16_t port, std::string path)
{
    http::client         client{sched};
    http::client_request req{
        .method  = http::request_method::GET,
        .version = {.major = 1, .minor = 1},
        .uri     = net::url::parse("http://127.0.0.1:" + std::to_string(port) + path).value(),
    };

    auto resp = co_await client.send(std::move(req));
    if (!resp.has_value())
        co_return response{.status = http::status::NONE, .connection = "", .body = "error: " + resp.error().message()};

    response out{
        .status     = resp->status_code,
        .connection = std::string{resp->headers.get("Connection"sv).value_or("")},
        .body       = "",
    };

    std::array<std::byte, 256> buf{};
    while (true)
    {
        auto res = co_await resp->body->read(buf);
        out.body.append(reinterpret_cast<const char*>(buf.data()), res.count);
        if (res.err) break;
    }

    co_return out;
}

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

// count counts the occurrences of what in str.
std::size_t count(std::string_view str, std::string_view what)
{
    std::size_t n = 0;
    for (auto pos = str.find(what); pos != std::string_view::npos; pos = str.find(what, pos + what.size())) ++n;
    return n;
}
}

TEST_CASE("the server responds for handlers that don't", "[http][server]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    http::router router;
    router.GET("/silent", [](const http::server_request&, http::response_writer&) -> task<void> { co_return; });
    router.GET("/throws",
               [](const http::server_request&, http::response_writer& resp) -> task<void>
               {
                   resp.headers().set("X-Partial"sv, "yes"sv);
                   throw std::runtime_error{"oops"};
                   co_return;
               });

    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    SECTION("an empty 200 OK, if the handler didn't send a response")
    {
        auto resp = sched.run_to_completion(get(&sched, server.local_port(), "/silent"));
        CHECK(resp.status == http::status::OK);
        CHECK(resp.body.empty());
    }

    SECTION("a 500 Internal Server Error, closing the connection, if the handler threw")
    {
        auto resp = sched.run_to_completion(get(&sched, server.local_port(), "/throws"));
        CHECK(resp.status == http::status::INTERNAL_SERVER_ERROR);
        CHECK(resp.connection == "close");
        CHECK(resp.body.empty());
    }
}

TEST_CASE("a server serves with the listener it's given", "[http][server]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    http::router router;
    router.GET("/hello",
               [](const http::server_request&, http::response_writer& resp) -> task<void>
               {
                   resp.headers().set("Content-Type", "text/plain");
                   auto* body = co_await resp.send(http::status::OK, 5);
                   co_await net::io::write_all(*body, "hello"sv);
               });

    http::server server{&sched, std::move(*listener), std::move(router)};
    REQUIRE(sched.spawn(server.serve()));

    auto resp = sched.run_to_completion(get(&sched, server.local_port(), "/hello"));
    CHECK(resp.status == http::status::OK);
    CHECK(resp.body == "hello");
}

TEST_CASE("a response's body needn't have a known length", "[http][server]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    http::router router;
    router.GET("/stream",
               [](const http::server_request&, http::response_writer& resp) -> task<void>
               {
                   auto* body = co_await resp.send(http::status::OK);
                   co_await net::io::write_all(*body, "hello"sv);
                   co_await net::io::write_all(*body, ", world"sv);
               });

    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    SECTION("chunked, to an HTTP/1.1 client, which can reuse the connection")
    {
        auto resp = sched.run_to_completion(get(&sched, server.local_port(), "/stream"));
        CHECK(resp.status == http::status::OK);
        CHECK(resp.connection.empty());
        CHECK(resp.body == "hello, world");
    }

    SECTION("in chunks, as the handler wrote them")
    {
        auto raw = sched.run_to_completion(
            exchange(&sched, server.local_port(), "GET /stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
        CHECK(raw.contains("\r\nTransfer-Encoding: chunked\r\n"));
        CHECK_FALSE(raw.contains("Content-Length"));
        CHECK(raw.ends_with("\r\n\r\n5\r\nhello\r\n7\r\n, world\r\n0\r\n\r\n"));
    }

    SECTION("ended by closing the connection, to an HTTP/1.0 client")
    {
        auto raw = sched.run_to_completion(exchange(&sched, server.local_port(), "GET /stream HTTP/1.0\r\n\r\n"));
        CHECK(raw.starts_with("HTTP/1.0 200 OK\r\n"));
        CHECK(raw.contains("\r\nConnection: close\r\n"));
        CHECK_FALSE(raw.contains("Transfer-Encoding"));
        CHECK_FALSE(raw.contains("Content-Length"));
        CHECK(raw.ends_with("\r\n\r\nhello, world"));
    }

    SECTION("without a body, for HEAD")
    {
        auto raw = sched.run_to_completion(
            exchange(&sched, server.local_port(), "HEAD /stream HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
        CHECK(raw.contains("\r\nTransfer-Encoding: chunked\r\n"));
        CHECK(raw.ends_with("\r\n\r\n"));
    }
}

TEST_CASE("responses to HEAD requests have no body", "[http][server]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    http::router router;
    router.GET("/hello",
               [](const http::server_request&, http::response_writer& resp) -> task<void>
               {
                   auto* body = co_await resp.send(http::status::OK, 5);
                   co_await net::io::write_all(*body, "hello"sv);
               });

    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    // a body after the HEAD response would be read as the start of the next response.
    auto out = sched.run_to_completion(exchange(&sched,
                                                server.local_port(),
                                                "HEAD /hello HTTP/1.1\r\nHost: x\r\n\r\n"
                                                "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    CAPTURE(out);

    CHECK(count(out, "HTTP/1.1 200 OK\r\n") == 2);
    CHECK(count(out, "Content-Length: 5\r\n") == 2);
    CHECK(count(out, "hello") == 1);
    CHECK(out.ends_with("\r\n\r\nhello"));
}

TEST_CASE("the server sends the date", "[http][server]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    http::router router;
    router.GET("/", [](const http::server_request&, http::response_writer&) -> task<void> { co_return; });

    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    // for a handler's response, and for the server's own (here, to a request it can't decode).
    auto out = sched.run_to_completion(exchange(&sched,
                                                server.local_port(),
                                                "GET / HTTP/1.1\r\nHost: x\r\n\r\n"
                                                "GET / HTTP/1.1\r\n\r\n"));
    CAPTURE(out);

    CHECK(count(out, "HTTP/1.1 200 OK\r\n") == 1);
    CHECK(count(out, "HTTP/1.1 400 Bad Request\r\n") == 1);
    CHECK(count(out, "\r\nDate: ") == 2);
    CHECK(count(out, " GMT\r\n") == 2);
}

TEST_CASE("a response's body is as long as its Content-Length", "[http][server]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    std::error_code long_err;

    http::router router;
    router
        .GET("/short",
             [](const http::server_request&, http::response_writer& resp) -> task<void>
             {
                 auto* body = co_await resp.send(http::status::OK, 10);
                 co_await net::io::write_all(*body, "hello"sv);
             })
        .GET("/long",
             [&](const http::server_request&, http::response_writer& resp) -> task<void>
             {
                 auto* body = co_await resp.send(http::status::OK, 5);
                 long_err   = (co_await net::io::write_all(*body, "hello, world"sv)).err;
             })
        .GET("/ok",
             [](const http::server_request&, http::response_writer& resp) -> task<void>
             {
                 auto* body = co_await resp.send(http::status::OK, 2);
                 co_await net::io::write_all(*body, "ok"sv);
             });

    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    SECTION("a body that's too short closes the connection, rather than leave the client waiting")
    {
        auto out = sched.run_to_completion(exchange(&sched,
                                                    server.local_port(),
                                                    "GET /short HTTP/1.1\r\nHost: x\r\n\r\n"
                                                    "GET /ok HTTP/1.1\r\nHost: x\r\n\r\n"));
        CAPTURE(out);

        CHECK(out.ends_with("\r\n\r\nhello"));
        CHECK(count(out, "HTTP/1.1 200 OK\r\n") == 1);
    }

    SECTION("what's too long isn't written, so the connection can still be used")
    {
        auto out = sched.run_to_completion(exchange(&sched,
                                                    server.local_port(),
                                                    "GET /long HTTP/1.1\r\nHost: x\r\n\r\n"
                                                    "GET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
        CAPTURE(out);

        CHECK(long_err == std::errc::value_too_large);
        CHECK(count(out, "HTTP/1.1 200 OK\r\n") == 2);
        CHECK(count(out, "world") == 0);
        CHECK(out.ends_with("\r\n\r\nok"));
    }
}

namespace
{

// echo_name responds with the route's :name.
task<void> echo_name(const http::server_request& req, http::response_writer& resp)
{
    const auto name = req.path_value("name");
    auto*      body = co_await resp.send(http::status::OK, name.size());
    co_await net::io::write_all(*body, name);
}

}

TEST_CASE("pipelined requests are answered in order", "[http][server][pipelining]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    http::router router;
    router.GET("/:name", echo_name);

    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    // the last request has no Host, so the server answers it with a 400, and closes the connection.
    auto out = sched.run_to_completion(exchange(&sched,
                                                server.local_port(),
                                                "GET /first HTTP/1.1\r\nHost: x\r\n\r\n"
                                                "GET /second HTTP/1.1\r\nHost: x\r\n\r\n"
                                                "GET /third HTTP/1.1\r\nHost: x\r\n\r\n"
                                                "GET / HTTP/1.1\r\n\r\n"));
    CAPTURE(out);

    CHECK(count(out, "HTTP/1.1 200 OK\r\n") == 3);

    const auto first  = out.find("\r\n\r\nfirst");
    const auto second = out.find("\r\n\r\nsecond");
    const auto third  = out.find("\r\n\r\nthird");
    const auto last   = out.find("HTTP/1.1 400 Bad Request\r\n");
    CHECK(first < second);
    CHECK(second < third);
    CHECK(third < last);
    CHECK(last != std::string::npos);
}

TEST_CASE("a response isn't held back waiting for the next request", "[http][server][pipelining]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    http::router router;
    router.GET("/:name", echo_name);

    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    // the server holds a response until it reads from the connection again (to answer pipelined requests together),
    // so it must send it as it waits for the next request.
    const auto exchange_one = [&] -> task<std::string>
    {
        auto conn = co_await net::tcp_socket::connect(&sched, "127.0.0.1", std::to_string(server.local_port()));
        if (!conn.has_value()) co_return "error: " + conn.error().message();
        conn->set_read_timeout(2s);

        auto res = co_await net::io::write_all(*conn, "GET /one HTTP/1.1\r\nHost: x\r\n\r\n"sv);
        if (res.err) co_return "error: " + res.err.message();

        std::string                out;
        std::array<std::byte, 256> buf{};
        while (!out.ends_with("\r\n\r\none"))
        {
            auto read = co_await conn->read(buf);
            out.append(reinterpret_cast<const char*>(buf.data()), read.count);
            if (read.err) co_return out + "error: " + read.err.message();
        }

        co_return out;
    };

    auto out = sched.run_to_completion(exchange_one());
    CAPTURE(out);

    CHECK(out.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(out.ends_with("\r\n\r\none"));
}

TEST_CASE("the server skips bodies handlers don't read, and closes connections it can't continue", "[http][server]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    const auto throws = [](const http::server_request& /*req*/, http::response_writer& /*resp*/) -> task<void>
    {
        throw std::runtime_error{"oops"};
        co_return;
    };

    http::router router;
    router.GET("/throws", throws);
    router.GET("/:name", echo_name);
    router.POST("/:name", echo_name);

    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    SECTION("a body the handler doesn't read is skipped, and the next request answered")
    {
        // the second request has no Host, so the server answers it with a 400, and closes the connection.
        auto out = sched.run_to_completion(exchange(&sched,
                                                    server.local_port(),
                                                    "POST /first HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\n\r\nabcdef"
                                                    "GET /second HTTP/1.1\r\nHost: x\r\n\r\n"
                                                    "GET / HTTP/1.1\r\n\r\n"));
        CAPTURE(out);

        CHECK(count(out, "HTTP/1.1 200 OK\r\n") == 2);
        CHECK(out.find("\r\n\r\nfirst") < out.find("\r\n\r\nsecond"));
        CHECK(out.contains("HTTP/1.1 400 Bad Request\r\n"));
    }

    SECTION("an HTTP version that isn't implemented gets a 505")
    {
        auto out = sched.run_to_completion(exchange(&sched, server.local_port(), "GET /a HTTP/2.0\r\nHost: x\r\n\r\n"));
        CAPTURE(out);

        CHECK(out.starts_with("HTTP/1.1 505 HTTP Version Not Supported\r\n"));
        CHECK(out.contains("\r\nConnection: close\r\n"));
    }

    SECTION("a handler that throws gets a 500, and the connection closed")
    {
        auto out = sched.run_to_completion(exchange(&sched,
                                                    server.local_port(),
                                                    "GET /throws HTTP/1.1\r\nHost: x\r\n\r\n"
                                                    "GET /never HTTP/1.1\r\nHost: x\r\n\r\n"));
        CAPTURE(out);

        CHECK(out.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
        CHECK(out.contains("\r\nConnection: close\r\n"));
        CHECK_FALSE(out.contains("never"));
    }
}

TEST_CASE("a request body cut short by the client fails to read, rather than ending", "[http][server][body]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    // the handler answers with what reading the body found: all of it, or what of it came, and why it stopped.
    const auto read_body = [](http::server_request& req, http::response_writer& resp) -> task<void>
    {
        auto body = co_await net::io::read_all(*req.body);

        std::string out = body.has_value() ? "whole: " + *body : "failed: " + body.error().message();
        if (!body.has_value() && body.error() == std::errc::connection_aborted) out = "cut short";

        auto* writer = co_await resp.send(http::status::OK, out.size());
        co_await net::io::write_all(*writer, out);
    };

    http::router router;
    router.POST("/", read_body);

    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    // the client sends 5 of the body's 10 bytes, and ends its side of the connection (still reading the response).
    const auto send_short = [](net::io::scheduler* sched, std::uint16_t port) -> task<std::string>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
        if (!conn.has_value()) co_return "error: " + conn.error().message();
        conn->set_read_timeout(5s);

        auto res =
            co_await net::io::write_all(*conn, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nhello"sv);
        if (res.err) co_return "error: " + res.err.message();
        conn->shutdown_write();

        std::string                out;
        std::array<std::byte, 256> buf{};
        while (true)
        {
            auto read = co_await conn->read(buf);
            out.append(reinterpret_cast<const char*>(buf.data()), read.count);
            if (read.err) break;
        }
        co_return out;
    };

    auto out = sched.run_to_completion(send_short(&sched, server.local_port()));
    CAPTURE(out);

    // and the server closed the connection after the response (which the read above waited for).
    CHECK(out.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(out.ends_with("\r\n\r\ncut short"));
}

TEST_CASE("the server follows the request's version and Connection header", "[http][server]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    // a HEAD handler that says how long the GET's body is, as it should, without writing it.
    const auto head = [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
    { co_await resp.send(http::status::OK, 5); };

    http::router router;
    router.HEAD("/h", head);
    router.GET("/:name", echo_name);

    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    // NOTE: a request without a Host ends a connection that the server keeps open, with a 400 (see the pipelining
    //       tests).
    constexpr auto end = "GET / HTTP/1.1\r\n\r\n"sv;

    SECTION("Connection: close closes it after the response")
    {
        auto out = sched.run_to_completion(exchange(&sched,
                                                    server.local_port(),
                                                    "GET /a HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
                                                    "GET /b HTTP/1.1\r\nHost: x\r\n\r\n"));
        CAPTURE(out);

        CHECK(count(out, "HTTP/1.1 200 OK\r\n") == 1);
        CHECK(out.contains("\r\nConnection: close\r\n"));
        CHECK(out.ends_with("\r\n\r\na"));
    }

    SECTION("HTTP/1.0 closes it")
    {
        auto out = sched.run_to_completion(
            exchange(&sched, server.local_port(), "GET /a HTTP/1.0\r\n\r\nGET /b HTTP/1.0\r\n\r\n"));
        CAPTURE(out);

        CHECK(out.starts_with("HTTP/1.0 200 OK\r\n"));
        CHECK(out.contains("\r\nConnection: close\r\n"));
        CHECK(count(out, "200 OK") == 1);
    }

    SECTION("HTTP/1.0 keeps it, if the client asks")
    {
        auto out = sched.run_to_completion(exchange(&sched,
                                                    server.local_port(),
                                                    "GET /a HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"
                                                    "GET /b HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"
                                                        + std::string{end}));
        CAPTURE(out);

        CHECK(count(out, "HTTP/1.0 200 OK\r\n") == 2);
        CHECK(out.contains("\r\nConnection: keep-alive\r\n"));
    }

    SECTION("an Upgrade is ignored: the request is handled as usual")
    {
        auto out = sched.run_to_completion(
            exchange(&sched,
                     server.local_port(),
                     "GET /a HTTP/1.1\r\nHost: x\r\nUpgrade: HTTP/1.1\r\nConnection: upgrade\r\n\r\n"
                     "GET /b HTTP/1.1\r\nHost: x\r\n\r\n"
                         + std::string{end}));
        CAPTURE(out);

        CHECK_FALSE(out.contains("101"));
        CHECK(count(out, "HTTP/1.1 200 OK\r\n") == 2);
        CHECK(out.find("\r\n\r\na") < out.find("\r\n\r\nb"));
    }

    SECTION("a HEAD response's Content-Length is the GET's, without a body")
    {
        auto out = sched.run_to_completion(exchange(&sched,
                                                    server.local_port(),
                                                    "HEAD /h HTTP/1.1\r\nHost: x\r\n\r\n"
                                                    "GET /b HTTP/1.1\r\nHost: x\r\n\r\n"
                                                        + std::string{end}));
        CAPTURE(out);

        CHECK(out.contains("\r\nContent-Length: 5\r\n"));
        CHECK(count(out, "HTTP/1.1 200 OK\r\n") == 2);
        CHECK(out.contains("\r\n\r\nb"));
    }

    SECTION("a response's version is the highest the server implements")
    {
        auto out = sched.run_to_completion(
            exchange(&sched, server.local_port(), "GET /a HTTP/1.9\r\nHost: x\r\n\r\n" + std::string{end}));
        CAPTURE(out);

        CHECK(out.starts_with("HTTP/1.1 200 OK\r\n"));
        CHECK_FALSE(out.contains("HTTP/1.9"));
    }
}

namespace
{

// blocking_client is a plain, blocking connection to the server, for tests that need a client on its own thread (e.g.
// one that doesn't read).
class blocking_client
{
public:
    explicit blocking_client(std::uint16_t port)
        : fd{::socket(AF_INET, SOCK_STREAM, 0)}
    {
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API.
        REQUIRE(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
        set_receive_timeout(5s);
    }

    blocking_client(const blocking_client&)            = delete;
    blocking_client& operator=(const blocking_client&) = delete;
    blocking_client(blocking_client&&)                 = delete;
    blocking_client& operator=(blocking_client&&)      = delete;

    ~blocking_client() { close(); }

    void close() noexcept
    {
        if (fd != -1) ::close(std::exchange(fd, -1));
    }

    // set_receive_timeout limits how long a read waits (then reading nothing).
    void set_receive_timeout(std::chrono::milliseconds timeout) const
    {
        timeval tv{.tv_sec  = static_cast<decltype(timeval::tv_sec)>(timeout.count() / 1'000),
                   .tv_usec = static_cast<decltype(timeval::tv_usec)>((timeout.count() % 1'000) * 1'000)};
        REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    }

    void send(std::string_view data) const
    {
        REQUIRE(::send(fd, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size()));
    }

    // read_some reads what's there, or waits for some (up to the receive timeout): nothing if none came, or the
    // server closed the connection.
    [[nodiscard]] std::string read_some() const
    {
        std::array<char, 4'096> buf{};
        auto                    n = ::recv(fd, buf.data(), buf.size(), 0);
        return n > 0 ? std::string(buf.data(), static_cast<std::size_t>(n)) : std::string{};
    }

    // read_to_end reads until the server closes the connection (or a read times out), returning what it read.
    [[nodiscard]] std::string read_to_end() const
    {
        std::string out;
        while (true)
        {
            auto chunk = read_some();
            if (chunk.empty()) return out;
            out += chunk;
        }
    }

private:
    int fd;
};

// running_scheduler runs a scheduler on its own thread, for tests that use blocking clients.
struct running_scheduler
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    std::thread        runner{[this] { sched.run(); }};

    running_scheduler()                                    = default;
    running_scheduler(const running_scheduler&)            = delete;
    running_scheduler& operator=(const running_scheduler&) = delete;
    running_scheduler(running_scheduler&&)                 = delete;
    running_scheduler& operator=(running_scheduler&&)      = delete;

    ~running_scheduler()
    {
        sched.shutdown();
        runner.join();
    }
};

}

TEST_CASE("a server can be destroyed while the scheduler runs", "[http][server][lifetime]")
{
    running_scheduler running;

    http::router router;
    router.GET("/:name", echo_name);

    auto server = std::make_unique<http::server>(&running.sched,
                                                 std::move(router),
                                                 http::server_config{.host = "127.0.0.1", .port = "0"});
    REQUIRE(running.sched.spawn(server->serve()));

    // one connection waits for its next request, after one.
    blocking_client idle{server->local_port()};
    idle.send("GET /a HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(idle.read_some().ends_with("\r\n\r\na"));

    // it ends the connections, and waits for them (and serve()) to finish.
    auto start = std::chrono::steady_clock::now();
    server.reset();
    CHECK(std::chrono::steady_clock::now() - start < 2s);

    CHECK(idle.read_to_end().empty());
}

TEST_CASE("a server can be destroyed before its serve() task starts", "[http][server][lifetime]")
{
    // one worker, kept busy for a moment: serve()'s task waits behind it.
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};

    const auto busy = [](std::chrono::milliseconds how_long) -> task<>
    {
        // NOTE: blocking the worker, deliberately.
        std::this_thread::sleep_for(how_long);
        co_return;
    };

    const auto router = []
    {
        http::router r;
        r.GET("/:name", echo_name);
        return r;
    };

    SECTION("destroyed")
    {
        auto server =
            std::make_unique<http::server>(&sched, router(), http::server_config{.host = "127.0.0.1", .port = "0"});
        REQUIRE(sched.spawn(busy(200ms)));
        REQUIRE(sched.spawn(server->serve()));

        // it waits for the task, which returns at once when it runs (rather than serving a destroyed server).
        const auto start = std::chrono::steady_clock::now();
        server.reset();
        CHECK(std::chrono::steady_clock::now() - start < 2s);
    }

    SECTION("closed before it starts: it doesn't serve")
    {
        http::server server{
            &sched,
            router(),
            {.host = "127.0.0.1", .port = "0"}
        };
        REQUIRE(sched.spawn(busy(100ms)));
        REQUIRE(sched.spawn(server.serve()));
        server.close();

        // the connection's queued (the server listens from when it's constructed), but not served.
        blocking_client conn{server.local_port()};
        conn.set_receive_timeout(500ms);
        conn.send("GET /a HTTP/1.1\r\nHost: x\r\n\r\n");
        CHECK(conn.read_some().empty());
    }
}

TEST_CASE("the server closes connections that make no progress", "[http][server][timeout]")
{
    running_scheduler running;

    // a handler with a long body.
    const std::string long_body(std::size_t{64} * 1'024 * 1'024, 'x');
    const auto        lots = [&](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
    {
        auto* body = co_await resp.send(http::status::OK, long_body.size());
        co_await net::io::write_all(*body, long_body);
    };

    http::router router;
    router.GET("/lots", lots);
    router.POST("/:name", echo_name);

    http::server server{
        &running.sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0", .body_read_timeout = 1s, .write_timeout = 1s}
    };
    REQUIRE(running.sched.spawn(server.serve()));

    SECTION("a body that doesn't come")
    {
        blocking_client conn{server.local_port()};
        conn.send("POST /a HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n");

        auto start = std::chrono::steady_clock::now();
        auto out   = conn.read_to_end();
        CAPTURE(out);

        // the server answers (the handler doesn't read the body), then gives up skipping the rest of it.
        CHECK(out.starts_with("HTTP/1.1 200 OK\r\n"));
        CHECK(std::chrono::steady_clock::now() - start >= 1s);
        CHECK(std::chrono::steady_clock::now() - start < 4s);
    }

    SECTION("a client that doesn't read the response")
    {
        blocking_client conn{server.local_port()};
        conn.send("GET /lots HTTP/1.1\r\nHost: x\r\n\r\n");

        // had the server waited, the client would get all of it now.
        std::this_thread::sleep_for(3s);
        auto out = conn.read_to_end();

        CHECK(out.starts_with("HTTP/1.1 200 OK\r\n"));
        CHECK(out.size() < long_body.size());
    }
}

TEST_CASE("a cleartext connection that sends nothing is closed at header_read_timeout", "[http][server][timeout]")
{
    running_scheduler running;

    http::router router;
    router.GET("/:name", echo_name);

    // NOTE: with HTTP/2 on (the default), the server first looks for its preface, within the same deadline.
    http::server server{
        &running.sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0", .header_read_timeout = 1s}
    };
    REQUIRE(running.sched.spawn(server.serve()));

    blocking_client conn{server.local_port()};

    const auto start = std::chrono::steady_clock::now();
    CHECK(conn.read_to_end().empty());

    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(took >= 900ms);
    CHECK(took < 1'800ms);
}

TEST_CASE("the server serves at most max_connections at once", "[http][server][limits]")
{
    running_scheduler running;

    http::router router;
    router.GET("/:name", echo_name);

    http::server server{
        &running.sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0", .max_connections = 1}
    };
    REQUIRE(running.sched.spawn(server.serve()));

    blocking_client first{server.local_port()};
    first.send("GET /first HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(first.read_some().ends_with("\r\n\r\nfirst"));

    // the second connects (the kernel accepts it), but isn't served while the first is open.
    blocking_client second{server.local_port()};
    second.send("GET /second HTTP/1.1\r\nHost: x\r\n\r\n");
    second.set_receive_timeout(300ms);
    CHECK(second.read_some().empty());

    first.close();
    second.set_receive_timeout(5s);
    CHECK(second.read_some().ends_with("\r\n\r\nsecond"));
}

TEST_CASE("connections made before the server serves are queued, not refused", "[http][server]")
{
    running_scheduler running;

    http::router router;
    router.GET("/:name", echo_name);

    http::server server{
        &running.sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };

    // connects (and sends a request) before serve() has even been spawned.
    blocking_client conn{server.local_port()};
    conn.send("GET /early HTTP/1.1\r\nHost: x\r\n\r\n");

    REQUIRE(running.sched.spawn(server.serve()));
    CHECK(conn.read_some().ends_with("\r\n\r\nearly"));
}

TEST_CASE("the server backs off when accepting fails, e.g. as it's out of file descriptors", "[http][server][limits]")
{
    // counts the server's failures to accept, which it logs.
    std::atomic<int> failures = 0;
    auto             sink     = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [&](const spdlog::details::log_msg& msg)
        {
            if (std::string_view{msg.payload.data(), msg.payload.size()}.starts_with("failed to accept")) ++failures;
        });

    running_scheduler running;

    http::router router;
    router.GET("/:name", echo_name);

    http::server server{
        &running.sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0", .logger = std::make_shared<spdlog::logger>("accept-backoff", sink)}
    };
    REQUIRE(running.sched.spawn(server.serve()));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(server.local_port());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // the client's socket is made first, as no file can be opened once the limit's lowered.
    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(client != -1);
    net::util::defer close_client{[&] noexcept { ::close(client); }};

    {
        // the process may only have the files it has open now: the lowest free descriptor is the limit.
        const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(probe != -1);
        ::close(probe);

        rlimit original{};
        REQUIRE(::getrlimit(RLIMIT_NOFILE, &original) == 0);

        rlimit lowered   = original;
        lowered.rlim_cur = static_cast<rlim_t>(probe);
        REQUIRE(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);
        net::util::defer restore{[&] noexcept { ::setrlimit(RLIMIT_NOFILE, &original); }};

        // the connection is made (the kernel accepts it), but the server can't accept it: there's no descriptor left.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API.
        REQUIRE(::connect(client, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
        std::this_thread::sleep_for(300ms);
    }

    // backing off (from 5ms, doubling) tries a few times in 300ms (Linux leaves the connection queued, so it fails each
    // time; macOS drops it after the first): retrying straight away would be thousands of times.
    CHECK(failures.load() >= 1);
    CHECK(failures.load() <= 10);

    // and once descriptors are free again, it serves.
    blocking_client later{server.local_port()};
    later.send("GET /later HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(later.read_some().ends_with("\r\n\r\nlater"));
}

namespace
{

// flag is set once, by one task, for others to wait for.
struct flag
{
    void set(net::io::scheduler* sched)
    {
        word.store(1);
        sched->notify_all(word);
    }

    // wait waits (up to 5s) for the flag to be set, returning whether it was.
    task<bool> wait(net::io::scheduler* sched)
    {
        const auto deadline = net::io::clock::now() + 5s;
        while (word.load() == 0)
        {
            if (co_await sched->wait_on(word, 0, deadline)) co_return word.load() != 0;
        }
        co_return true;
    }

    std::atomic<std::uint64_t> word = 0;
};

// serving runs a server's serve() task until the server's closed, then sets done (which must outlive the scheduler,
// as this may still be setting it after the server's destroyed). It takes the task, rather than calling serve() when
// it starts: the server waits for the task from when serve() is called, and could otherwise be destroyed first.
task<> serving(net::io::scheduler* sched, task<> serve, flag* done)
{
    co_await std::move(serve);
    done->set(sched);
}

// read_to_end reads conn until it ends (or fails), returning what it read and why it stopped.
task<std::pair<std::string, std::error_code>> read_to_end(net::io::reader& conn)
{
    std::string                out;
    std::array<std::byte, 256> buf{};
    while (true)
    {
        auto read = co_await conn.read(buf);
        out.append(reinterpret_cast<const char*>(buf.data()), read.count);
        if (read.err) co_return std::pair{out, read.err};
    }
}

// read_body answers with what reading the request's body found: all of it, or why it stopped.
task<void> read_body(http::server_request& req, http::response_writer& resp)
{
    auto body = co_await net::io::read_all(*req.body);

    const std::string out    = body.has_value() ? "whole: " + *body : "failed: " + body.error().message();
    auto*             writer = co_await resp.send(http::status::OK, out.size());
    co_await net::io::write_all(*writer, out);
}

}

TEST_CASE("closing the server lets the requests in progress finish, then closes their connections",
          "[http][server][close]")
{
    // NOTE: before the scheduler, which waits for the tasks that use them.
    flag started;
    flag release;
    flag serve_done;

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/:name", echo_name);

    // reads its body once it's started.
    router.POST("/read",
                [&](http::server_request& req, http::response_writer& resp) -> task<void>
                {
                    started.set(&sched);
                    co_await read_body(req, resp);
                });

    // waits to be released, before reading its body.
    router.POST("/wait",
                [&](http::server_request& req, http::response_writer& resp) -> task<void>
                {
                    started.set(&sched);
                    co_await release.wait(&sched);
                    co_await read_body(req, resp);
                });

    // sends its response's head, then waits to be released, before sending its body.
    router.POST("/head-first",
                [&](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
                {
                    auto* body = co_await resp.send(http::status::OK, 5);
                    started.set(&sched);
                    co_await release.wait(&sched);
                    co_await net::io::write_all(*body, "hello"sv);
                });

    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(serving(&sched, server.serve(), &serve_done)));

    struct outcome
    {
        std::string     received;
        std::error_code err;
        bool            served_all = false;
    };

    SECTION("a body still coming is read whole, and the response says the connection closes")
    {
        const auto test = [&] -> task<outcome>
        {
            auto conn = co_await net::tcp_socket::connect(&sched, "127.0.0.1", std::to_string(server.local_port()));
            REQUIRE(conn.has_value());
            conn->set_read_timeout(5s);

            // half the body, then the rest once the server's closed, as its handler reads it.
            (void)co_await net::io::write_all(*conn,
                                              "POST /read HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nhello"sv);
            const bool handled = co_await started.wait(&sched);
            server.close();
            (void)co_await net::io::write_all(*conn, "world"sv);

            auto [received, err] = co_await read_to_end(*conn);
            const bool done      = co_await serve_done.wait(&sched);
            co_return outcome{.received = received, .err = err, .served_all = handled && done};
        };

        auto out = sched.run_to_completion(test());
        CAPTURE(out.received, out.err.message());

        CHECK(out.received.starts_with("HTTP/1.1 200 OK\r\n"));
        CHECK(out.received.contains("\r\nConnection: close\r\n"));
        CHECK(out.received.ends_with("\r\n\r\nwhole: helloworld"));
        CHECK(out.err == net::io::errc::end_of_stream);
        CHECK(out.served_all);
    }

    SECTION("requests pipelined after it aren't served")
    {
        const auto test = [&] -> task<outcome>
        {
            auto conn = co_await net::tcp_socket::connect(&sched, "127.0.0.1", std::to_string(server.local_port()));
            REQUIRE(conn.has_value());
            conn->set_read_timeout(5s);

            (void)co_await net::io::write_all(*conn,
                                              "POST /wait HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
                                              "GET /second HTTP/1.1\r\nHost: x\r\n\r\n"sv);
            const bool handled = co_await started.wait(&sched);
            server.close();
            release.set(&sched);

            auto [received, err] = co_await read_to_end(*conn);
            const bool done      = co_await serve_done.wait(&sched);
            co_return outcome{.received = received, .err = err, .served_all = handled && done};
        };

        auto out = sched.run_to_completion(test());
        CAPTURE(out.received, out.err.message());

        CHECK(count(out.received, "HTTP/1.1 ") == 1);
        CHECK(out.received.contains("\r\nConnection: close\r\n"));
        CHECK(out.received.ends_with("\r\n\r\nwhole: hello"));
        CHECK_FALSE(out.received.contains("second"));
        CHECK(out.err == net::io::errc::end_of_stream);
        CHECK(out.served_all);
    }

    SECTION("a response whose head was sent before goes on, then the connection closes")
    {
        const auto test = [&] -> task<outcome>
        {
            auto conn = co_await net::tcp_socket::connect(&sched, "127.0.0.1", std::to_string(server.local_port()));
            REQUIRE(conn.has_value());
            conn->set_read_timeout(5s);

            (void)co_await net::io::write_all(*conn,
                                              "POST /head-first HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"
                                              "GET /second HTTP/1.1\r\nHost: x\r\n\r\n"sv);
            const bool handled = co_await started.wait(&sched);
            server.close();
            release.set(&sched);

            auto [received, err] = co_await read_to_end(*conn);
            const bool done      = co_await serve_done.wait(&sched);
            co_return outcome{.received = received, .err = err, .served_all = handled && done};
        };

        auto out = sched.run_to_completion(test());
        CAPTURE(out.received, out.err.message());

        CHECK(count(out.received, "HTTP/1.1 ") == 1);
        CHECK_FALSE(out.received.contains("\r\nConnection: close\r\n"));
        CHECK(out.received.ends_with("\r\n\r\nhello"));
        CHECK_FALSE(out.received.contains("second"));
        CHECK(out.err == net::io::errc::end_of_stream);
        CHECK(out.served_all);
    }
}

TEST_CASE("closing the server closes idle connections at once", "[http][server][close]")
{
    // NOTE: before the scheduler, which waits for the task that sets it.
    flag serve_done;

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/:name", echo_name);

    // NOTE: a long header_read_timeout, so that only close() ends the idle connections soon.
    http::server server{
        &sched,
        std::move(router),
        {.host = "127.0.0.1", .port = "0", .header_read_timeout = 30s}
    };

    REQUIRE(sched.spawn(serving(&sched, server.serve(), &serve_done)));

    struct outcome
    {
        std::string              silent;
        std::error_code          silent_err;
        std::string              kept;
        std::error_code          kept_err;
        bool                     served_all = false;
        std::chrono::nanoseconds took{};
    };

    const auto test = [&] -> task<outcome>
    {
        const auto port = std::to_string(server.local_port());

        // one waits for its first request (accepted before the next, which the server has answered); one for its
        // next, after one.
        auto silent = co_await net::tcp_socket::connect(&sched, "127.0.0.1", port);
        auto kept   = co_await net::tcp_socket::connect(&sched, "127.0.0.1", port);
        REQUIRE(silent.has_value());
        REQUIRE(kept.has_value());
        silent->set_read_timeout(5s);
        kept->set_read_timeout(5s);

        (void)co_await net::io::write_all(*kept, "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"sv);
        std::string                answered;
        std::array<std::byte, 256> buf{};
        while (!answered.ends_with("\r\n\r\na"))
        {
            auto read = co_await kept->read(buf);
            answered.append(reinterpret_cast<const char*>(buf.data()), read.count);
            if (read.err) break;
        }

        const auto start = std::chrono::steady_clock::now();
        server.close();

        outcome out;
        std::tie(out.silent, out.silent_err) = co_await read_to_end(*silent);
        std::tie(out.kept, out.kept_err)     = co_await read_to_end(*kept);
        out.served_all                       = co_await serve_done.wait(&sched);
        out.took                             = std::chrono::steady_clock::now() - start;
        co_return out;
    };

    auto out = sched.run_to_completion(test());
    CAPTURE(out.silent, out.silent_err.message(), out.kept, out.kept_err.message());

    CHECK(out.silent.empty());
    CHECK(out.silent_err == net::io::status_condition::closed);
    CHECK(out.kept.empty());
    CHECK(out.kept_err == net::io::status_condition::closed);
    CHECK(out.served_all);
    CHECK(out.took < 1s);
}

namespace
{

// close_after closes server after delay, then sets closed.
task<> close_after(net::io::scheduler* sched, http::server* server, net::io::clock::duration delay, flag* closed)
{
    (void)co_await sched->sleep_until(net::io::clock::now() + delay);
    server->close();
    closed->set(sched);
}

// race_close sends a request to the server on port (a kept-alive connection's second), whose body comes in two parts,
// while the server's closed after close_delay: returning what the client received for it.
task<std::string> race_close(net::io::scheduler*      sched,
                             http::server*            server,
                             net::io::clock::duration close_delay,
                             net::io::clock::duration body_delay)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(server->local_port()));
    if (!conn.has_value()) co_return "error: " + conn.error().message();
    conn->set_read_timeout(5s);

    (void)co_await net::io::write_all(*conn, "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"sv);
    std::string                answered;
    std::array<std::byte, 256> buf{};
    while (!answered.ends_with("\r\n\r\na"))
    {
        auto read = co_await conn->read(buf);
        answered.append(reinterpret_cast<const char*>(buf.data()), read.count);
        if (read.err) co_return "error: " + read.err.message();
    }

    flag closed;
    if (!sched->spawn(close_after(sched, server, close_delay, &closed))) co_return "error: not spawned";

    // NOTE: once the server's closed, these may fail (e.g. reset): it's what the client receives that matters.
    (void)co_await net::io::write_all(*conn, "POST /read HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nhello"sv);
    (void)co_await sched->sleep_until(net::io::clock::now() + body_delay);
    (void)co_await net::io::write_all(*conn, "world"sv);

    auto [received, err] = co_await read_to_end(*conn);
    if (!co_await closed.wait(sched)) co_return "error: not closed";
    co_return received;
}

}

TEST_CASE("closing the server as a request arrives either serves it whole or not at all", "[http][server][close]")
{
    // the server's closed at different points around the request: before its head (the connection's idle, and closed
    // at once), while its body's coming, or after its response.
    for (int i = 0; i < 40; ++i)
    {
        net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

        http::router router;
        router.GET("/:name", echo_name);
        router.POST("/read", read_body);

        http::server server{
            &sched,
            std::move(router),
            {.host = "127.0.0.1", .port = "0"}
        };
        REQUIRE(sched.spawn(server.serve()));

        const auto close_delay = std::chrono::microseconds{(i % 10) * 200};
        auto       received    = sched.run_to_completion(race_close(&sched, &server, close_delay, 1ms));
        CAPTURE(i, received);

        // nothing (the connection closed), or the whole body read.
        CHECK_FALSE(received.starts_with("error: "));
        if (!received.empty())
        {
            CHECK(received.starts_with("HTTP/1.1 200 OK\r\n"));
            CHECK(received.ends_with("\r\n\r\nwhole: helloworld"));
        }
    }
}

TEST_CASE("destroying a server ends the requests in progress that close() lets finish", "[http][server][lifetime]")
{
    running_scheduler running;

    std::atomic<bool> started = false;

    http::router router;
    router.POST("/read",
                [&](http::server_request& req, http::response_writer& resp) -> task<void>
                {
                    started.store(true);
                    co_await read_body(req, resp);
                });

    auto server = std::make_unique<http::server>(&running.sched,
                                                 std::move(router),
                                                 http::server_config{.host = "127.0.0.1", .port = "0"});
    REQUIRE(running.sched.spawn(server->serve()));

    // the body doesn't come (for the default body_read_timeout, a minute).
    blocking_client conn{server->local_port()};
    conn.send("POST /read HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nhello");

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!started.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    REQUIRE(started.load());

    const auto start = std::chrono::steady_clock::now();
    server.reset();
    CHECK(std::chrono::steady_clock::now() - start < 2s);

    // the handler's read failed, and it answered, as the connection could still take it.
    auto out = conn.read_to_end();
    CAPTURE(out);
    CHECK(out.contains("failed: "));
}
