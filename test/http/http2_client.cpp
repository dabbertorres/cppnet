#include "http/http2_client.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/connection.hpp"
#include "http/connection_pool.hpp"
#include "http/http.hpp"
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
#include "tcp.hpp"
#include "url.hpp"

namespace http = net::http;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;

namespace
{

task<void> hello(const http::server_request& req, http::response_writer& resp)
{
    const auto msg  = std::format("hello over HTTP/{}", req.version.major);
    auto*      body = co_await resp.send(http::status::OK, msg.size());
    co_await net::io::write_all(*body, msg);
}

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
              (void)co_await sched->sleep_until(net::io::clock::now() + 200ms);
              auto* body = co_await resp.send(http::status::OK, 4);
              co_await net::io::write_all(*body, "slow"sv);
          });
    r.GET("/big",
          [](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              const std::string big(1'000'000, 'b');
              auto*             body = co_await resp.send(http::status::OK, big.size());
              co_await net::io::write_all(*body, big);
          });
    r.GET("/huge",
          [](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              const std::string huge(4'000'000, 'h');
              auto*             body = co_await resp.send(http::status::OK, huge.size());
              co_await net::io::write_all(*body, huge);
          });
    return r;
}

struct test_server
{
    explicit test_server(net::io::scheduler* sched, const http::server_config& cfg = {})
        : server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), make_router(sched), cfg}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    [[nodiscard]] std::string url(std::string_view path) const
    {
        return std::format("http://127.0.0.1:{}{}", server.local_port(), path);
    }

    http::server server;
};

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

http::client_options prior_knowledge()
{
    http::client_options options;
    options.http2_prior_knowledge = true;
    return options;
}

// fetch GETs url, returning the response's version and body, or the error.
task<std::string> fetch(http::client* client, std::string url)
{
    auto resp = co_await client->get(url);
    if (!resp.has_value()) co_return "error: " + resp.error().message();

    auto body = co_await net::io::read_all(*resp->body);
    if (!body.has_value()) co_return "error: " + body.error().message();
    co_return std::format("{} {}", resp->version.major, *body);
}

}

TEST_CASE("the client speaks HTTP/2 with prior knowledge", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};
    http::client       client{&sched, prior_knowledge()};

    struct outcome
    {
        std::string            first;
        std::string            second;
        http::connection_stats stats;
    };

    // NOTE: the stats while the scheduler runs, as stopping it ends the connection.
    const auto test = [](http::client* client, std::string url) -> task<outcome>
    {
        auto first  = co_await fetch(client, url);
        auto second = co_await fetch(client, url);
        co_return outcome{.first = first, .second = second, .stats = client->stats()};
    };

    const auto out = sched.run_to_completion(test(&client, server.url("/")));
    CHECK(out.first == "2 hello over HTTP/2");
    CHECK(out.second == "2 hello over HTTP/2");

    // one connection, kept, with no streams left: idle.
    CHECK(out.stats == http::connection_stats{.open = 1, .idle = 1});
}

TEST_CASE("the client's concurrent requests share one HTTP/2 connection", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};
    http::client       client{&sched, prior_knowledge()};

    const auto test =
        [](net::io::scheduler* sched, http::client* client, std::string url) -> task<std::vector<std::string>>
    {
        // the first opens the connection; the rest go at once.
        std::vector<std::string> out{co_await fetch(client, url)};

        constexpr int              n = 20;
        std::vector<std::string>   results(n);
        std::atomic<std::uint64_t> done{0};
        for (int i = 0; i < n; ++i)
        {
            REQUIRE(sched->spawn(
                [](net::io::scheduler*         sched,
                   http::client*               client,
                   std::string                 url,
                   std::string*                result,
                   std::atomic<std::uint64_t>* done) -> task<>
                {
                    *result = co_await fetch(client, url);
                    done->fetch_add(1);
                    sched->notify_all(*done);
                }(sched, client, url, &results[static_cast<std::size_t>(i)], &done)));
        }

        // until they've all finished.
        for (auto seen = done.load(); seen < n; seen = done.load()) (void)co_await sched->wait_on(done, seen);
        out.insert(out.end(), results.begin(), results.end());
        out.push_back(std::format("open {}", client->stats().open));
        co_return out;
    };

    const auto results = sched.run_to_completion(test(&sched, &client, server.url("/slow")));
    REQUIRE(results.size() == 22);
    for (std::size_t i = 0; i < 21; ++i) CHECK(results[i] == "2 slow");
    CHECK(results.back() == "open 1");
}

TEST_CASE("the client's HTTP/2 bodies larger than the windows go both ways", "[http][http2][client][flow]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};
    http::client       client{&sched, prior_knowledge()};

    std::string body(300'000, 'x');
    for (std::size_t i = 0; i < body.size(); ++i) body[i] = static_cast<char>('a' + (i % 26));

    const auto test = [](http::client* client, std::string url, std::string body) -> task<std::string>
    {
        http::client_request req{.method = http::request_method::POST, .uri = net::url::parse(url).value()};
        req.set_body(std::move(body));

        auto resp = co_await client->send(std::move(req));
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        auto received = co_await net::io::read_all(*resp->body);
        if (!received.has_value()) co_return "error: " + received.error().message();
        co_return *received;
    };

    CHECK(sched.run_to_completion(test(&client, server.url("/echo"), body)) == body);
}

TEST_CASE("a response dropped before its end resets its stream, not the connection", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};
    http::client       client{&sched, prior_knowledge()};

    const auto test = [](http::client* client, test_server* server) -> task<std::string>
    {
        {
            auto resp = co_await client->get(server->url("/big"));
            REQUIRE(resp.has_value());
            // dropped, unread.
        }

        auto result = co_await fetch(client, server->url("/"));
        co_return std::format("{}, open {}", result, client->stats().open);
    };

    CHECK(sched.run_to_completion(test(&client, &server)) == "2 hello over HTTP/2, open 1");
}

TEST_CASE("responses dropped partly read give the HTTP/2 connection's window back", "[http][http2][client][flow]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<std::string>
    {
        auto socket = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
        REQUIRE(socket.has_value());

        // a connection whose window is 4 streams' (the client's is 1 GiB), which 30 responses' unread data (at least a
        // frame of 16 KiB each) would use up.
        http::detail::http2_client_options options{.io_timeout = 5s};
        options.session.connection_window = 4 * 65'535;

        auto conn = std::make_shared<http::detail::http2_client_connection>(
            sched,
            std::make_unique<http::detail::connection>(std::move(*socket)),
            options,
            http::detail::http2_client_connection::callbacks{});
        REQUIRE(conn->start());

        const auto big = net::url::parse(std::format("http://127.0.0.1:{}/big", port)).value();
        for (int i = 0; i < 30; ++i)
        {
            http::client_request req{.method = http::request_method::GET, .uri = big};
            auto                 resp = co_await conn->send(req);
            REQUIRE(resp.has_value());

            std::array<std::byte, 1> one{};
            REQUIRE_FALSE((co_await resp->body->read(one)).err);
            // dropped, mostly unread.
        }

        // then one read to its end: at once, rather than a byte at a time (all the window that would be left).
        http::client_request req{.method = http::request_method::GET, .uri = big};
        auto                 resp = co_await conn->send(req);
        REQUIRE(resp.has_value());

        const auto             deadline = net::io::clock::now() + 5s;
        std::size_t            total    = 0;
        std::vector<std::byte> buffer(65'536);
        std::error_code        err;
        while (!err && net::io::clock::now() < deadline)
        {
            const auto res = co_await resp->body->read(buffer);
            total += res.count;
            err = res.err;
        }
        resp->body.reset();
        conn->close();

        if (err && err != net::io::errc::end_of_stream) co_return "error: " + err.message();
        co_return std::format("read {}", total);
    };

    CHECK(sched.run_to_completion(test(&sched, server.server.local_port())) == "read 1000000");
}

TEST_CASE("many large HTTP/2 responses can be read one at a time", "[http][http2][client][flow]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    auto options       = prior_knowledge();
    options.io_timeout = 5s;
    http::client client{&sched, options};

    const auto test = [](http::client* client, std::string url) -> task<std::vector<std::string>>
    {
        // 24 responses' heads, and as much of their bodies as their streams' windows allow (1.5 MiB in all); then each
        // read to the end in turn.
        constexpr int                      n = 24;
        std::vector<http::client_response> responses;
        for (int i = 0; i < n; ++i)
        {
            auto resp = co_await client->get(url);
            REQUIRE(resp.has_value());
            responses.push_back(std::move(*resp));
        }

        std::vector<std::string> out;
        for (auto& resp : responses)
        {
            auto body = co_await net::io::read_all(*resp.body);
            out.push_back(body.has_value() ? std::format("read {}", body->size()) : "error: " + body.error().message());
        }
        out.push_back(std::format("open {}", client->stats().open));
        co_return out;
    };

    const auto results = sched.run_to_completion(test(&client, server.url("/huge")));
    REQUIRE(results.size() == 25);
    for (std::size_t i = 0; i < 24; ++i) CHECK(results[i] == "read 4000000");
    CHECK(results.back() == "open 1");
}

TEST_CASE("at the server's stream limit, the client opens another connection, or waits",
          "[http][http2][client][limits]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, {.http2_max_concurrent_streams = 2}};

    const bool strict = GENERATE(false, true);
    CAPTURE(strict);

    auto options                          = prior_knowledge();
    options.strict_max_concurrent_streams = strict;
    http::client client{&sched, options};

    const auto test =
        [](net::io::scheduler* sched, http::client* client, std::string url) -> task<std::vector<std::string>>
    {
        // the first makes the connection (and learns the limit), then 6 at once.
        std::vector<std::string> out{co_await fetch(client, url)};

        constexpr int              n = 6;
        std::vector<std::string>   results(n);
        std::atomic<std::uint64_t> done{0};
        for (int i = 0; i < n; ++i)
        {
            REQUIRE(sched->spawn(
                [](net::io::scheduler*         sched,
                   http::client*               client,
                   std::string                 url,
                   std::string*                result,
                   std::atomic<std::uint64_t>* done) -> task<>
                {
                    *result = co_await fetch(client, url);
                    done->fetch_add(1);
                    sched->notify_all(*done);
                }(sched, client, url, &results[static_cast<std::size_t>(i)], &done)));
        }

        // until they've all finished.
        for (auto seen = done.load(); seen < n; seen = done.load()) (void)co_await sched->wait_on(done, seen);
        out.insert(out.end(), results.begin(), results.end());
        out.push_back(std::to_string(client->stats().open));
        co_return out;
    };

    const auto results = sched.run_to_completion(test(&sched, &client, server.url("/slow")));
    REQUIRE(results.size() == 8);
    for (std::size_t i = 0; i < 7; ++i) CHECK(results[i] == "2 slow");

    const auto open = std::stoul(results.back());
    if (strict) CHECK(open == 1);
    else CHECK(open > 1);
}

TEST_CASE("an idle HTTP/2 connection is closed after idle_timeout", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    auto options         = prior_knowledge();
    options.idle_timeout = 200ms;
    http::client client{&sched, options};

    const auto test = [](net::io::scheduler* sched, http::client* client, std::string url) -> task<std::string>
    {
        auto result = co_await fetch(client, url);

        // until it's closed (a second at most).
        for (int i = 0; i < 100 && client->stats().open != 0; ++i)
            (void)co_await sched->sleep_until(net::io::clock::now() + 10ms);
        co_return result;
    };

    CHECK(sched.run_to_completion(test(&sched, &client, server.url("/"))) == "2 hello over HTTP/2");
    CHECK(client.stats() == http::connection_stats{});
}

TEST_CASE("without http2, the client speaks HTTP/1.1 even with prior knowledge", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    auto options  = prior_knowledge();
    options.http2 = false;
    http::client client{&sched, options};

    CHECK(sched.run_to_completion(fetch(&client, server.url("/"))) == "1 hello over HTTP/1");
}
