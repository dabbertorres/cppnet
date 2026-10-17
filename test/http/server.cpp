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
#include <thread>
#include <utility>

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>

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
