#include "http/server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/callback_sink.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/connection_pool.hpp"
#include "http/h2_client.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "tls/test_pki.hpp"
#include "tls/tls.hpp"

namespace http = net::http;
namespace tls  = net::tls;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::pki;
using test::server_with;
using test::trusting_client;

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// hello says whether the request came over TLS.
task<void> hello(const http::server_request& req, http::response_writer& resp)
{
    const auto msg  = req.tls ? "hello over TLS"sv : "hello over TCP"sv;
    auto*      body = co_await resp.send(http::status::OK, msg.size());
    co_await net::io::write_all(*body, msg);
}

// log_watch keeps what a server logs, for a test to wait for.
struct log_watch
{
    explicit log_watch(net::io::scheduler* sched)
        : sched{sched}
    {
        logger->set_level(spdlog::level::trace);
    }

    // wait waits (up to 5s) until a line containing text has been logged, returning whether one was.
    task<bool> wait(std::string_view text)
    {
        const auto deadline = net::io::clock::now() + 5s;
        while (true)
        {
            const auto seen = count.load();
            if (contains(text)) co_return true;
            if (co_await sched->wait_on(count, seen, deadline)) co_return false;
        }
    }

    bool contains(std::string_view text)
    {
        std::scoped_lock lock{mu};
        return std::ranges::any_of(lines, [text](const std::string& line) { return line.contains(text); });
    }

    net::io::scheduler*        sched;
    std::mutex                 mu;
    std::vector<std::string>   lines;
    std::atomic<std::uint64_t> count = 0;

    std::shared_ptr<spdlog::logger> logger =
        std::make_shared<spdlog::logger>("tls-server",
                                         std::make_shared<spdlog::sinks::callback_sink_mt>(
                                             [this](const spdlog::details::log_msg& msg)
                                             {
                                                 {
                                                     std::scoped_lock lock{mu};
                                                     lines.emplace_back(msg.payload.data(), msg.payload.size());
                                                 }
                                                 count.fetch_add(1);
                                                 sched->notify_all(count);
                                             }));
};

// https_server is a server of hello, over TLS (if tls is set), on a port of its own.
struct https_server
{
    https_server(net::io::scheduler* sched, const http::server_config& cfg)
        : server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), router(), cfg}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    static http::router router()
    {
        http::router r;
        r.GET("/", hello);
        return r;
    }

    [[nodiscard]] std::string port() const { return std::to_string(server.local_port()); }

    http::server server;
};

http::server_config tls_config(std::chrono::seconds header_read_timeout = 5s)
{
    return {.header_read_timeout = header_read_timeout, .tls = server_with(pki().localhost_chain, pki().localhost_key)};
}

http::client_options trusting_options()
{
    http::client_options options;
    options.tls.trusted_ca_file = pki().ca;
    return options;
}

// fetch GETs url, returning the body, or the error.
task<std::string> fetch(http::client* client, std::string url)
{
    auto resp = co_await client->get(url);
    if (!resp.has_value()) co_return "error: " + resp.error().message();

    auto body = co_await net::io::read_all(*resp->body);
    co_return body.has_value() ? *body : "error: " + body.error().message();
}

// read_to_end reads s until it ends (or fails), returning what it read and why it stopped.
task<std::pair<std::string, std::error_code>> read_to_end(net::io::reader& s)
{
    std::string                  out;
    std::array<std::byte, 4'096> buf{};
    while (true)
    {
        auto res = co_await s.read(buf);
        out.append(reinterpret_cast<const char*>(buf.data()), res.count);
        if (res.err) co_return std::pair{out, res.err};
    }
}

}

TEST_CASE("an HTTPS server serves requests over TLS", "[tls][http][server]")
{
    net::io::scheduler sched{make_pool()};

    const bool use_tls = GENERATE(true, false);
    CAPTURE(use_tls);

    https_server      server{&sched, use_tls ? tls_config() : http::server_config{}};
    http::client      client{&sched, trusting_options()};
    const std::string url = (use_tls ? "https://localhost:" : "http://localhost:") + server.port() + "/";

    struct outcome
    {
        std::string            first;
        std::string            second;
        http::connection_stats stats;
    };

    // NOTE: the stats while the scheduler runs: stopping it ends an HTTP/2 connection (which TLS agrees on).
    const auto test = [](http::client* client, std::string url) -> task<outcome>
    {
        auto first  = co_await fetch(client, url);
        auto second = co_await fetch(client, url);
        co_return outcome{.first = first, .second = second, .stats = client->stats()};
    };

    const auto out = sched.run_to_completion(test(&client, url));

    const std::string_view expected = use_tls ? "hello over TLS" : "hello over TCP";
    CHECK(out.first == expected);
    CHECK(out.second == expected);

    // the connection was kept for the second request.
    CHECK(out.stats == http::connection_stats{.open = 1, .idle = 1});
}

TEST_CASE("the HTTP client speaks HTTP/2 to an HTTPS server that does", "[tls][http][client][http2]")
{
    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, tls_config()};
    http::client       client{&sched, trusting_options()};

    const auto test = [](http::client* client, std::string url) -> task<std::string>
    {
        auto resp = co_await client->get(url);
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        auto body = co_await net::io::read_all(*resp->body);
        if (!body.has_value()) co_return "error: " + body.error().message();
        co_return std::format("HTTP/{} {}", resp->version.major, *body);
    };

    CHECK(sched.run_to_completion(test(&client, "https://localhost:" + server.port() + "/"))
          == "HTTP/2 hello over TLS");
}

TEST_CASE("an HTTPS server closes connections that don't speak TLS", "[tls][http][server]")
{
    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, tls_config()};
    http::client       client{&sched, trusting_options()};

    const auto test = [](http::client* client, std::string port) -> task<std::pair<std::string, std::string>>
    {
        auto plain = co_await fetch(client, "http://localhost:" + port + "/");
        co_return std::pair{plain, co_await fetch(client, "https://localhost:" + port + "/")};
    };

    auto [plain, over_tls] = sched.run_to_completion(test(&client, server.port()));
    CHECK(plain.starts_with("error: "));

    // and still serves those that do.
    CHECK(over_tls == "hello over TLS");
}

TEST_CASE("an HTTPS server closes connections whose handshake doesn't finish in time", "[tls][http][server]")
{
    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, tls_config(1s)};

    // connects, but never starts the handshake.
    const auto test = [](net::io::scheduler* sched, std::string port) -> task<std::error_code>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        if (!conn.has_value()) co_return conn.error();

        std::array<std::byte, 64> buf{};
        auto                      res = co_await conn->read(buf);
        co_return res.err;
    };

    const auto start = std::chrono::steady_clock::now();
    auto       err   = sched.run_to_completion(test(&sched, server.port()));
    CAPTURE(err.message());
    CHECK(err == net::io::status_condition::closed);
    CHECK(std::chrono::steady_clock::now() - start < 4s);
}

TEST_CASE("closing an HTTPS server ends handshakes in progress", "[tls][http][server]")
{
    net::io::scheduler sched{make_pool()};
    log_watch          log{&sched};

    // without header_read_timeout, nothing but close() ends a handshake that doesn't finish.
    auto cfg   = tls_config(0s);
    cfg.logger = log.logger;
    https_server server{&sched, cfg};

    struct outcome
    {
        bool            accepted = false;
        std::error_code read_err;
        bool            canceled = false;
    };

    // connects, but never starts the handshake; then closes the server, once it has accepted the connection.
    const auto test = [](net::io::scheduler* sched, http::server* server, log_watch* log) -> task<outcome>
    {
        outcome out;

        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(server->local_port()));
        if (!conn.has_value())
        {
            out.read_err = conn.error();
            co_return out;
        }

        out.accepted = co_await log->wait("connection accepted");
        server->close();

        std::array<std::byte, 64> buf{};
        auto                      res = co_await conn->read(buf);
        out.read_err                  = res.err;
        out.canceled                  = co_await log->wait("TLS handshake failed: Operation canceled");
        co_return out;
    };

    const auto start = std::chrono::steady_clock::now();
    auto       out   = sched.run_to_completion(test(&sched, &server.server, &log));
    CAPTURE(out.read_err.message());
    CHECK(out.accepted);
    CHECK(out.read_err == net::io::status_condition::closed);
    CHECK(out.canceled);
    CHECK(std::chrono::steady_clock::now() - start < 4s);
}

TEST_CASE("an HTTPS server serves HTTP/2 to clients that agree on h2", "[tls][http][server][http2]")
{
    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, tls_config()};

    const auto test = [](net::io::scheduler* sched, std::string port) -> task<std::pair<std::string, std::string>>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        REQUIRE(conn.has_value());

        auto s = co_await tls::stream::connect(std::move(*conn),
                                               trusting_client({"h2", "http/1.1"}),
                                               "localhost",
                                               net::io::clock::now() + 5s);
        REQUIRE(s.has_value());
        if (s->alpn() != "h2") co_return std::pair{"ALPN: " + std::string{s->alpn()}, std::string{}};

        test::h2_client client{&*s, &*s};
        const auto      id = client.request("GET", "/");
        REQUIRE(co_await client.wait_for(id));

        co_await s->close();
        co_return std::pair{client.responses[id].status, client.responses[id].body};
    };

    auto [status, body] = sched.run_to_completion(test(&sched, server.port()));
    CHECK(status == "200");
    CHECK(body == "hello over TLS");
}

TEST_CASE("without http2, an HTTPS server doesn't agree on h2", "[tls][http][server][http2]")
{
    net::io::scheduler sched{make_pool()};

    auto cfg  = tls_config();
    cfg.http2 = false;
    https_server server{&sched, cfg};

    const auto test = [](net::io::scheduler* sched, std::string port) -> task<std::string>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        REQUIRE(conn.has_value());

        auto s = co_await tls::stream::connect(std::move(*conn),
                                               trusting_client({"h2"}),
                                               "localhost",
                                               net::io::clock::now() + 5s);
        REQUIRE(s.has_value());

        const std::string alpn{s->alpn()};
        co_await s->close();
        co_return alpn;
    };

    CHECK(sched.run_to_completion(test(&sched, server.port())).empty());
}

TEST_CASE("an HTTPS server's error responses end with close_notify", "[tls][http][server]")
{
    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, tls_config()};

    const auto test = [](net::io::scheduler* sched, std::string port) -> task<std::pair<std::string, std::error_code>>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        if (!conn.has_value()) co_return std::pair{"connect failed", conn.error()};

        auto s =
            co_await tls::stream::connect(std::move(*conn), trusting_client(), "localhost", net::io::clock::now() + 5s);
        if (!s.has_value()) co_return std::pair{"handshake failed", s.error()};

        co_await net::io::write_all(*s, "NOT HTTP\r\n\r\n"sv);
        co_return co_await read_to_end(*s);
    };

    auto [received, err] = sched.run_to_completion(test(&sched, server.port()));
    CAPTURE(received, err.message());
    CHECK(received.starts_with("HTTP/1.1 400 "));
    CHECK(err == net::io::errc::end_of_stream);
}
