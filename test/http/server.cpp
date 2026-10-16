#include "http/server.hpp"

#include <array>
#include <chrono>
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
#include <utility>

#include <catch2/catch_test_macros.hpp>

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
#include "io/writer.hpp"
#include "tcp.hpp"
#include "url.hpp"

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
        .uri     = net::url::parse("http://127.0.0.1:" + std::to_string(port) + path).to_value(),
    };

    auto resp = co_await client.send(req);
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

    std::error_condition long_err;

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
