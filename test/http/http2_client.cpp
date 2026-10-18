#include "http/http2_client.hpp"

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
#include "http/h2_client.hpp"
#include "http/hpack.hpp"
#include "http/http.hpp"
#include "http/http2.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/string_reader.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
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
    r.POST("/ignore-body",
           [](const http::server_request&, http::response_writer& resp) -> task<void>
           {
               auto* body = co_await resp.send(http::status::OK, 4);
               co_await net::io::write_all(*body, "done"sv);
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
        : test_server{sched, make_router(sched), cfg}
    {}

    test_server(net::io::scheduler* sched, http::router router, const http::server_config& cfg = {})
        : server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), std::move(router), cfg}
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

// outcome is what a request sent in the background came to: its response's status and body, or the error.
struct outcome
{
    std::atomic<std::uint64_t> finished{0};
    int                        status = 0;
    std::string                body;
    std::error_code            err;

    // wait waits (up to 5 seconds) for the request to have finished, returning whether it has.
    task<bool> wait(net::io::scheduler* sched)
    {
        const auto deadline = net::io::clock::now() + 5s;
        while (finished.load() == 0)
        {
            if (co_await sched->wait_on(finished, 0, deadline)) break;
        }
        co_return finished.load() != 0;
    }
};

// record records a response (reading its body), or why there's none, in out.
task<> record(net::io::scheduler* sched, std::expected<http::client_response, std::error_code> resp, outcome* out)
{
    if (!resp.has_value())
    {
        out->err = resp.error();
    }
    else
    {
        out->status = static_cast<int>(resp->status_code);
        auto body   = co_await net::io::read_all(*resp->body);
        if (body.has_value()) out->body = std::move(*body);
        else out->err = body.error();
    }

    out->finished.store(1);
    sched->notify_all(out->finished);
}

// send_request sends req on conn, recording what happens in out.
task<> send_request(net::io::scheduler*                                    sched,
                    std::shared_ptr<http::detail::http2_client_connection> conn,
                    http::client_request                                   req,
                    outcome*                                               out)
{
    auto resp = co_await conn->send(req);
    co_await record(sched, std::move(resp), out);
}

// send_get GETs path on conn, recording what happens in out.
task<> send_get(net::io::scheduler*                                    sched,
                std::shared_ptr<http::detail::http2_client_connection> conn,
                net::url                                               uri,
                outcome*                                               out)
{
    return send_request(sched, std::move(conn), {.method = http::request_method::GET, .uri = std::move(uri)}, out);
}

// send_with sends req with client, recording what happens in out; then shuts stop (if given) down, so that a test
// waiting for a connection the client didn't make fails, rather than waiting for ever.
task<> send_with(net::io::scheduler*  sched,
                 http::client*        client,
                 http::client_request req,
                 outcome*             out,
                 net::listener*       stop = nullptr)
{
    auto resp = co_await client->send(std::move(req));
    co_await record(sched, std::move(resp), out);
    if (stop != nullptr) stop->shutdown();
}

http::client_request request(http::request_method method, const std::string& url)
{
    return {.method = method, .uri = net::url::parse(url).value()};
}

// fake_host is a host that the client connects to, whose connections the test accepts, to play the server.
struct fake_host
{
    explicit fake_host(net::io::scheduler* sched)
        : listener{net::listener::make(sched, "127.0.0.1", "0").value()}
    {}

    [[nodiscard]] std::string url(std::string_view path) const
    {
        return std::format("http://127.0.0.1:{}{}", listener.local_port(), path);
    }

    net::listener listener;
};

// played is a connection the test accepted, and the server it plays on it (with test::h2_client, as a server).
struct played
{
    explicit played(net::tcp_socket accepted, const http2::session_config& config = {})
        : socket{std::move(accepted)}
        , server{&socket, &socket, &socket, http2::role::server, config}
    {}

    net::tcp_socket socket;
    test::h2_client server;
};

task<std::unique_ptr<played>> accept(fake_host* host, const http2::session_config& config = {})
{
    auto accepted = co_await host->listener.accept();
    REQUIRE(accepted.has_value());
    co_return std::make_unique<played>(std::move(*accepted), config);
}

// endless is a body that never ends.
struct endless final : net::io::reader
{
    task<net::io::result> read(std::span<std::byte> data) override
    {
        std::ranges::fill(data, std::byte{'x'});
        co_return net::io::result{.count = data.size()};
    }
    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }
};

// counting_reader is a body of size bytes, saying how often it's been read.
struct counting_reader final : net::io::reader
{
    explicit counting_reader(std::size_t size, std::atomic<int>* reads)
        : left{size}
        , reads{reads}
    {}

    task<net::io::result> read(std::span<std::byte> data) override
    {
        reads->fetch_add(1);
        const auto n = std::min(left, data.size());
        std::ranges::fill(data.first(n), std::byte{'x'});
        left -= n;
        co_return net::io::result{.count = n,
                                  .err = left == 0 ? make_error_code(net::io::errc::end_of_stream) : std::error_code{}};
    }
    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

    std::size_t       left;
    std::atomic<int>* reads;
};

// fake_server is an HTTP/2 client connection to a server the test plays (with test::h2_client, as a server).
struct fake_server
{
    net::tcp_socket                                        socket; // the server's side
    std::shared_ptr<http::detail::http2_client_connection> conn;
};

task<fake_server> connect_fake(net::io::scheduler* sched, const http::detail::http2_client_options& options)
{
    auto listener = net::listener::make(sched, "127.0.0.1", "0").value();
    auto client   = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(listener.local_port()));
    REQUIRE(client.has_value());
    auto accepted = co_await listener.accept();
    REQUIRE(accepted.has_value());

    auto conn = std::make_shared<http::detail::http2_client_connection>(
        sched,
        std::make_unique<http::detail::connection>(std::move(*client)),
        options,
        http::detail::http2_client_connection::callbacks{});
    REQUIRE(conn->start());
    co_return fake_server{.socket = std::move(*accepted), .conn = std::move(conn)};
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
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};

    const auto test = [](net::io::scheduler* sched, http::client* client, fake_host* host) -> task<>
    {
        // a response dropped before its end, then another request.
        outcome out;
        REQUIRE(sched->spawn(
            [](net::io::scheduler* sched, http::client* client, fake_host* host, outcome* out) -> task<>
            {
                {
                    auto resp = co_await client->get(host->url("/big"));
                    REQUIRE(resp.has_value());
                    // dropped, unread.
                }
                co_await send_with(sched,
                                   client,
                                   request(http::request_method::GET, host->url("/")),
                                   out,
                                   &host->listener);
            }(sched, client, host, &out)));

        auto  conn   = co_await accept(host);
        auto& server = conn->server;
        REQUIRE(co_await server.pump([&] { return server.responses.contains(1); }));
        server.respond(1, "200", "part of it", false);

        // the stream's reset (CANCEL), and the next request comes on the same connection.
        REQUIRE(co_await server.pump([&] { return static_cast<bool>(server.responses[1].reset); }));
        CHECK(server.responses[1].reset == http2::error_code::cancel);

        REQUIRE(co_await server.pump([&] { return server.responses.contains(3); }));
        server.respond(3, "200", "again");
        REQUIRE(co_await server.pump([] { return true; })); // (just written)

        REQUIRE(co_await out.wait(sched));
        CHECK(out.status == 200);
        CHECK(out.body == "again");
        CHECK_FALSE(server.closed);
    };

    sched.run_to_completion(test(&sched, &client, &host));
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

    struct outcome
    {
        std::string            result;
        http::connection_stats kept;   // once the response was read
        http::connection_stats closed; // once it's been closed
    };

    // NOTE: the stats while the scheduler runs, as stopping it ends the connection anyway.
    const auto test = [](net::io::scheduler* sched, http::client* client, std::string url) -> task<outcome>
    {
        outcome out{.result = co_await fetch(client, url), .kept = client->stats(), .closed = {}};

        // until it's closed (2 seconds at most).
        const auto deadline = net::io::clock::now() + 2s;
        while (client->stats().open != 0 && net::io::clock::now() < deadline)
            (void)co_await sched->sleep_until(net::io::clock::now() + 10ms);

        out.closed = client->stats();
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, &client, server.url("/")));
    CHECK(out.result == "2 hello over HTTP/2");
    CHECK(out.kept == http::connection_stats{.open = 1, .idle = 1});
    CHECK(out.closed == http::connection_stats{});
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

TEST_CASE("an HTTP/2 request that fails before its response's head frees its stream", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};

    const bool refused = GENERATE(true, false);
    CAPTURE(refused);

    const auto test = [](net::io::scheduler* sched, bool refused) -> task<>
    {
        // the connection's closed once it's idle: which it never is, if the stream's kept.
        http::detail::http2_client_options options{.idle_timeout = 200ms};
        if (!refused) options.response_header_timeout = 200ms;

        auto            fake = co_await connect_fake(sched, options);
        test::h2_client server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        outcome out;
        REQUIRE(sched->spawn(send_get(sched, fake.conn, net::url::parse("http://127.0.0.1/").value(), &out)));
        REQUIRE(co_await server.pump([&] { return server.responses.contains(1); }));

        // refused, or never answered.
        if (refused) server.s.reset(1, http2::error_code::refused_stream);

        REQUIRE(co_await server.pump([&] { return server.closed; }, 3s));
        CHECK(server.goaway);

        REQUIRE(co_await out.wait(sched));
        if (refused) CHECK(out.err == http2::error_code::refused_stream);
        else CHECK(out.err == std::errc::timed_out);
    };

    sched.run_to_completion(test(&sched, refused));
}

TEST_CASE("RST_STREAM(NO_ERROR) cuts a response short, unless it's complete", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};

    SECTION("before the response's end")
    {
        const auto test = [](net::io::scheduler* sched) -> task<>
        {
            auto            fake = co_await connect_fake(sched, {.io_timeout = 3s});
            test::h2_client server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

            outcome out;
            REQUIRE(sched->spawn(send_get(sched, fake.conn, net::url::parse("http://127.0.0.1/").value(), &out)));
            REQUIRE(co_await server.pump([&] { return server.responses.contains(1); }));

            const std::vector<net::http::hpack::field> head{
                {.name = ":status", .value = "200"}
            };
            REQUIRE_FALSE(server.s.submit_headers(1, head, false));
            REQUIRE(server.s.submit_data(1, std::as_bytes(std::span{"part"sv}), false) == 4U);
            server.s.reset(1, http2::error_code::no_error);
            REQUIRE(co_await server.pump([] { return true; })); // (just written)

            REQUIRE(co_await out.wait(sched));
            CHECK(out.status == 200);
            CHECK(out.err == std::errc::connection_reset);
            CHECK(out.err == net::io::status_condition::closed);
            fake.conn->close();
        };

        sched.run_to_completion(test(&sched));
    }

    SECTION("after it: it only stops the request's body")
    {
        test_server  server{&sched};
        http::client client{&sched, prior_knowledge()};

        const auto test = [](http::client* client, std::string url) -> task<std::string>
        {
            // more body than the stream's window, which the handler doesn't read: the server answers, then resets
            // the stream (NO_ERROR) to stop the rest.
            http::client_request req{.method = http::request_method::POST, .uri = net::url::parse(url).value()};
            req.set_body(std::string(300'000, 'x'));

            auto resp = co_await client->send(std::move(req));
            if (!resp.has_value()) co_return "error: " + resp.error().message();

            auto body = co_await net::io::read_all(*resp->body);
            if (!body.has_value()) co_return "error: " + body.error().message();
            co_return std::format("{} {}", static_cast<int>(resp->status_code), *body);
        };

        CHECK(sched.run_to_completion(test(&client, server.url("/ignore-body"))) == "200 done");
    }
}

TEST_CASE("a request waiting for an HTTP/2 stream gets one when a response ends, though it's not been read",
          "[http][http2][client][limits]")
{
    net::io::scheduler sched{make_pool()};
    test::gate         g{&sched};

    auto r = make_router(&sched);
    r.GET("/held",
          [&g](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              auto* body = co_await resp.send(http::status::OK, 4);
              co_await g.pass();
              co_await net::io::write_all(*body, "held"sv);
          });
    test_server server{&sched, std::move(r), {.http2_max_concurrent_streams = 1}};

    // the one stream is waited for (not past connect_timeout).
    auto options                          = prior_knowledge();
    options.strict_max_concurrent_streams = true;
    options.connect_timeout               = 2s;
    http::client client{&sched, options};

    const auto test = [](net::io::scheduler* sched, http::client* client, test::gate* g, test_server* server)
        -> task<std::vector<std::string>>
    {
        // the connection, and its limit.
        std::vector<std::string> out{co_await fetch(client, server->url("/"))};

        // a response whose head has arrived, and its body not yet; then a request, which waits for the stream.
        auto held = co_await client->get(server->url("/held"));
        REQUIRE(held.has_value());

        std::string                waiting;
        std::atomic<std::uint64_t> done{0};
        REQUIRE(sched->spawn(
            [](net::io::scheduler*         sched,
               http::client*               client,
               std::string                 url,
               std::string*                result,
               std::atomic<std::uint64_t>* done) -> task<>
            {
                *result = co_await fetch(client, url);
                done->store(1);
                sched->notify_all(*done);
            }(sched, client, server->url("/"), &waiting, &done)));

        // NOTE: a moment for the request to start waiting (else it finds the stream free, and the test proves less,
        //       but still passes).
        (void)co_await sched->sleep_until(net::io::clock::now() + 100ms);

        // the response ends, but isn't read (nor dropped) until the request that was waiting is done.
        g->open();
        while (done.load() == 0) (void)co_await sched->wait_on(done, 0);
        out.push_back(waiting);

        auto body = co_await net::io::read_all(*held->body);
        out.push_back(body.value_or("error"));
        co_return out;
    };

    const auto results = sched.run_to_completion(test(&sched, &client, &g, &server));
    CHECK(results == std::vector<std::string>{"2 hello over HTTP/2", "2 hello over HTTP/2", "held"});
}

TEST_CASE("a refused HTTP/2 request is sent again, on another connection", "[http][http2][client][retry]")
{
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};

    const bool goaway = GENERATE(true, false);
    CAPTURE(goaway);

    // a retry on the wrong connection would wait for its head, which never comes.
    auto options                    = prior_knowledge();
    options.response_header_timeout = 2s;
    http::client client{&sched, options};

    const auto test = [](net::io::scheduler* sched, http::client* client, fake_host* host, bool goaway) -> task<>
    {
        outcome out;
        REQUIRE(sched->spawn(
            send_with(sched, client, request(http::request_method::GET, host->url("/")), &out, &host->listener)));

        auto first = co_await accept(host);
        REQUIRE(co_await first->server.pump([&] { return first->server.responses.contains(1); }));

        // not processed: after GOAWAY's last stream, or refused.
        if (goaway) first->server.s.goaway(0, http2::error_code::no_error);
        else first->server.s.reset(1, http2::error_code::refused_stream);
        REQUIRE(co_await first->server.pump([] { return true; })); // (just written)

        auto second = co_await accept(host);
        REQUIRE(co_await second->server.pump([&] { return second->server.responses.contains(1); }));
        second->server.respond(1, "200", "again");
        REQUIRE(co_await second->server.pump([] { return true; }));

        REQUIRE(co_await out.wait(sched));
        CHECK(out.status == 200);
        CHECK(out.body == "again");

        // nothing more came on the first.
        if (!goaway)
        {
            REQUIRE(co_await first->server.sync());
            CHECK_FALSE(first->server.responses.contains(3));
        }
    };

    sched.run_to_completion(test(&sched, &client, &host, goaway));
}

TEST_CASE("a request safe to repeat is sent again if its HTTP/2 connection drops before its response",
          "[http][http2][client][retry]")
{
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};

    const auto method = GENERATE(http::request_method::GET, http::request_method::POST);
    CAPTURE(method);

    const auto test =
        [](net::io::scheduler* sched, http::client* client, fake_host* host, http::request_method method) -> task<>
    {
        outcome out;
        REQUIRE(sched->spawn(send_with(sched, client, request(method, host->url("/")), &out, &host->listener)));

        // the connection drops, without GOAWAY, once the request's arrived.
        {
            auto first = co_await accept(host);
            REQUIRE(co_await first->server.pump([&] { return first->server.responses.contains(1); }));
        }

        // NOTE: kept until the response has been read: closing a socket with data unread (e.g. the client's SETTINGS
        //       acknowledgement) resets the connection, and Linux then drops what it hasn't sent yet.
        std::unique_ptr<played> second;
        if (method == http::request_method::GET)
        {
            second = co_await accept(host);
            REQUIRE(co_await second->server.pump([&] { return second->server.responses.contains(1); }));
            second->server.respond(1, "200", "again");
            REQUIRE(co_await second->server.pump([] { return true; }));
        }

        REQUIRE(co_await out.wait(sched));
        if (method == http::request_method::GET)
        {
            CHECK(out.status == 200);
            CHECK(out.body == "again");
        }
        else
        {
            // it may have been processed.
            CHECK(out.err == std::errc::connection_aborted);
        }
    };

    sched.run_to_completion(test(&sched, &client, &host, method));
}

TEST_CASE("HTTP/2 responses time out: their heads, and their bodies' stalls", "[http][http2][client][timeout]")
{
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};

    const bool head = GENERATE(true, false);
    CAPTURE(head);

    auto options                    = prior_knowledge();
    options.response_header_timeout = 300ms;
    options.io_timeout              = 300ms;
    http::client client{&sched, options};

    const auto test = [](net::io::scheduler* sched, http::client* client, fake_host* host, bool head) -> task<>
    {
        outcome out;
        REQUIRE(sched->spawn(send_with(sched, client, request(http::request_method::GET, host->url("/")), &out)));

        auto  conn   = co_await accept(host);
        auto& server = conn->server;
        REQUIRE(co_await server.pump([&] { return server.responses.contains(1); }));
        if (!head) server.respond(1, "200", "part", false);

        // the client gives up on the stream (alone).
        REQUIRE(co_await server.pump([&] { return static_cast<bool>(server.responses[1].reset); }, 3s));
        CHECK(server.responses[1].reset == http2::error_code::cancel);
        CHECK_FALSE(server.closed);

        REQUIRE(co_await out.wait(sched));
        CHECK(out.err == std::errc::timed_out);
        CHECK(out.status == (head ? 0 : 200));
    };

    sched.run_to_completion(test(&sched, &client, &host, head));
}

TEST_CASE("an HTTP/2 connection whose writes stall fails, with its streams", "[http][http2][client][timeout]")
{
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};

    // nothing but io_timeout limits the second request, waiting for its head.
    auto options                    = prior_knowledge();
    options.response_header_timeout = 0ms;
    options.io_timeout              = 300ms;
    http::client client{&sched, options};

    const auto test = [](net::io::scheduler* sched, http::client* client, fake_host* host) -> task<>
    {
        // a body that never ends, and a request after it (not safe to repeat, so it isn't, on another connection).
        const auto endless_post = [](const std::string& url)
        {
            auto req = request(http::request_method::POST, url);
            req.body = std::make_unique<endless>();
            return req;
        };

        outcome first;
        outcome second;
        REQUIRE(sched->spawn(send_with(sched, client, endless_post(host->url("/endless")), &first)));

        // windows that don't stop the body: only the server not reading does.
        http2::session_config config;
        config.local.initial_window_size = 0x7fff'ffff;
        config.connection_window         = 0x7fff'ffff;

        auto  conn   = co_await accept(host, config);
        auto& server = conn->server;
        REQUIRE(co_await server.pump([&] { return server.responses.contains(1); }));
        REQUIRE(
            sched->spawn(send_with(sched, client, request(http::request_method::POST, host->url("/after")), &second)));
        REQUIRE(co_await server.pump([&] { return server.responses.contains(3); }));

        // the server stops reading: the client's writer stalls, and fails the connection.
        REQUIRE(co_await second.wait(sched));
        CHECK(second.err == std::errc::connection_aborted);
        REQUIRE(co_await first.wait(sched));
        CHECK(first.err);
    };

    sched.run_to_completion(test(&sched, &client, &host));
}

TEST_CASE("destroying the client doesn't cut off the HTTP/2 bodies being read", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    auto client = std::make_unique<http::client>(&sched, prior_knowledge());

    const auto test = [](std::unique_ptr<http::client>* client, std::string url) -> task<std::string>
    {
        auto resp = co_await (*client)->get(url);
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        // NOTE: not awaited in REQUIRE: GCC evaluates a co_await in Catch's (never evaluated) loop condition too, which
        //       would read another byte.
        std::array<std::byte, 1> one{};
        const auto               first = co_await resp->body->read(one);
        REQUIRE(first.count == 1);
        REQUIRE_FALSE(first.err);

        client->reset();

        auto rest = co_await net::io::read_all(*resp->body);
        if (!rest.has_value()) co_return "error: " + rest.error().message();
        co_return std::format("read {}", rest->size() + 1);
    };

    CHECK(sched.run_to_completion(test(&client, server.url("/big"))) == "read 1000000");
}

TEST_CASE("at max_connections, an idle HTTP/2 connection to another host is closed to make room",
          "[http][http2][client][limits]")
{
    net::io::scheduler sched{make_pool()};
    test_server        a{&sched};
    test_server        b{&sched};

    auto options            = prior_knowledge();
    options.max_connections = 1;
    options.connect_timeout = 2s;
    http::client client{&sched, options};

    const auto test = [](http::client* client, test_server* a, test_server* b) -> task<std::vector<std::string>>
    {
        std::vector<std::string> out{co_await fetch(client, a->url("/")), co_await fetch(client, b->url("/"))};

        // the first is counted out at once, though it closes in its own time.
        const auto port = [](test_server* s) { return std::to_string(s->server.local_port()); };
        out.push_back(std::format("open {}, to a {}, to b {}",
                                  client->stats().open,
                                  client->stats("127.0.0.1", port(a)).open,
                                  client->stats("127.0.0.1", port(b)).open));
        co_return out;
    };

    CHECK(sched.run_to_completion(test(&client, &a, &b))
          == std::vector<std::string>{"2 hello over HTTP/2", "2 hello over HTTP/2", "open 1, to a 0, to b 1"});
}

TEST_CASE("a request's Host is its :authority over HTTP/2", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};

    struct test_case
    {
        std::string_view uri;
        std::string_view host; // the request's Host, if any
        std::string_view authority;
    };

    const auto tc = GENERATE(
        test_case{.uri = "http://example.com/", .host = "other.example:8080", .authority = "other.example:8080"},
        test_case{.uri = "http://example.com:80/", .host = "", .authority = "example.com"},
        test_case{.uri = "http://example.com:8080/", .host = "", .authority = "example.com:8080"},
        test_case{.uri = "https://example.com:443/", .host = "", .authority = "example.com"},
        test_case{.uri = "https://example.com:80/", .host = "", .authority = "example.com:80"},
        test_case{.uri = "http://[::1]:80/", .host = "", .authority = "[::1]"});
    CAPTURE(tc.uri, tc.host);

    const auto test = [](net::io::scheduler* sched, test_case tc) -> task<>
    {
        auto            fake = co_await connect_fake(sched, {});
        test::h2_client server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        const auto with_host = [](const test_case& tc)
        {
            http::client_request req{.method = http::request_method::GET, .uri = net::url::parse(tc.uri).value()};
            if (!tc.host.empty()) req.headers.set("Host", tc.host);
            return req;
        };

        outcome out;
        REQUIRE(sched->spawn(send_request(sched, fake.conn, with_host(tc), &out)));
        REQUIRE(co_await server.pump([&] { return server.responses.contains(1); }));

        const auto& r = server.responses[1];
        CHECK(test::header(r, ":authority") == tc.authority);
        CHECK(test::header(r, "host") == "(none)");

        server.respond(1, "204");
        REQUIRE(co_await server.pump([] { return true; }));
        REQUIRE(co_await out.wait(sched));
        CHECK(out.status == 204);
        fake.conn->close();
    };

    sched.run_to_completion(test(&sched, tc));
}

TEST_CASE("an HTTP/2 request's body is sent to its Content-Length", "[http][http2][client][body]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};
    http::client       client{&sched, prior_knowledge()};

    struct test_case
    {
        std::size_t      length;
        std::string_view body;
        std::string_view expected;
    };

    const auto tc = GENERATE(test_case{.length = 3, .body = "abcdef", .expected = "200 abc"},
                             test_case{.length = 10, .body = "abc", .expected = "error: Invalid argument"},
                             test_case{.length = 0, .body = "abc", .expected = "200 "});
    CAPTURE(tc.length, tc.body);

    const auto test = [](http::client* client, std::string url, test_case tc) -> task<std::string>
    {
        http::client_request req{.method = http::request_method::POST, .uri = net::url::parse(url).value()};
        req.headers.set_content_length(tc.length);
        req.body = std::make_unique<net::io::owned_string_reader>(std::string{tc.body});

        auto resp = co_await client->send(std::move(req));
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        auto body = co_await net::io::read_all(*resp->body);
        if (!body.has_value()) co_return "error: " + body.error().message();
        co_return std::format("{} {}", static_cast<int>(resp->status_code), *body);
    };

    CHECK(sched.run_to_completion(test(&client, server.url("/echo"), tc)) == tc.expected);
}

TEST_CASE("an HTTP/2 request's body that returns nothing fails", "[http][http2][client][body]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};
    http::client       client{&sched, prior_knowledge()};

    // nothing, without failing, a while; then the end.
    struct nothing final : net::io::reader
    {
        task<net::io::result> read(std::span<std::byte> /*data*/) override
        {
            if (--left == 0) co_return net::io::result{.err = make_error_code(net::io::errc::end_of_stream)};
            co_return net::io::result{};
        }
        using net::io::reader::read;

        [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

        int left = 1'000;
    };

    const auto test = [](http::client* client, std::string url) -> task<std::error_code>
    {
        http::client_request req{.method = http::request_method::POST, .uri = net::url::parse(url).value()};
        req.body = std::make_unique<nothing>();

        auto resp = co_await client->send(std::move(req));
        co_return resp.has_value() ? std::error_code{} : resp.error();
    };

    CHECK(sched.run_to_completion(test(&client, server.url("/echo"))) == net::io::errc::no_progress);
}

TEST_CASE("a strict request doesn't wait for a stream of an HTTP/2 connection that's draining",
          "[http][http2][client][limits]")
{
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};

    auto options                          = prior_knowledge();
    options.strict_max_concurrent_streams = true;
    options.connect_timeout               = 2s;
    http::client client{&sched, options};

    const auto test = [](net::io::scheduler* sched, http::client* client, fake_host* host) -> task<>
    {
        // a connection of one stream, which a response holds.
        http2::session_config config;
        config.local.max_concurrent_streams = 1;

        outcome held;
        REQUIRE(sched->spawn(send_with(sched, client, request(http::request_method::GET, host->url("/held")), &held)));

        auto first = co_await accept(host, config);
        REQUIRE(co_await first->server.pump([&] { return first->server.responses.contains(1); }));
        first->server.respond(1, "200", "held", false);
        REQUIRE(co_await first->server.sync());

        // a request waiting for its stream.
        outcome waiting;
        REQUIRE(sched->spawn(
            send_with(sched, client, request(http::request_method::GET, host->url("/")), &waiting, &host->listener)));

        // NOTE: a moment for the request to start waiting (else it finds the connection draining, and the test proves
        //       less, but still passes).
        (void)co_await sched->sleep_until(net::io::clock::now() + 100ms);

        // the server's done with the connection, once its stream is: the request goes elsewhere.
        first->server.s.goaway(1, http2::error_code::no_error);
        REQUIRE(co_await first->server.pump([] { return true; }));

        auto second = co_await accept(host, config);
        REQUIRE(co_await second->server.pump([&] { return second->server.responses.contains(1); }));
        second->server.respond(1, "200", "elsewhere");
        REQUIRE(co_await second->server.pump([] { return true; }));
        REQUIRE(co_await waiting.wait(sched));
        CHECK(waiting.body == "elsewhere");

        // and the first stream finishes as it was.
        first->server.send(1, "", true);
        REQUIRE(co_await first->server.pump([] { return true; }));
        REQUIRE(co_await held.wait(sched));
        CHECK(held.body == "held");
    };

    sched.run_to_completion(test(&sched, &client, &host));
}

TEST_CASE("a burst of requests to an HTTP/2 host shares the connection being made", "[http][http2][client][limits]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};
    http::client       client{&sched, prior_knowledge()};

    const auto test =
        [](net::io::scheduler* sched, http::client* client, std::string url) -> task<std::vector<std::string>>
    {
        // all at once, with no connection yet (prior knowledge says it speaks HTTP/2).
        constexpr int              n = 10;
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

        for (auto seen = done.load(); seen < n; seen = done.load()) (void)co_await sched->wait_on(done, seen);
        results.push_back(std::format("open {}", client->stats().open));
        co_return results;
    };

    const auto results = sched.run_to_completion(test(&sched, &client, server.url("/")));
    REQUIRE(results.size() == 11);
    for (std::size_t i = 0; i < 10; ++i) CHECK(results[i] == "2 hello over HTTP/2");
    CHECK(results.back() == "open 1");
}

TEST_CASE("with idle_timeout 0, an HTTP/2 connection closes once its last stream is done", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};

    auto options         = prior_knowledge();
    options.idle_timeout = 0ms;
    http::client client{&sched, options};

    const auto test = [](net::io::scheduler* sched, http::client* client, fake_host* host) -> task<>
    {
        outcome out;
        REQUIRE(sched->spawn(send_with(sched, client, request(http::request_method::GET, host->url("/")), &out)));

        auto  conn   = co_await accept(host);
        auto& server = conn->server;
        REQUIRE(co_await server.pump([&] { return server.responses.contains(1); }));
        server.respond(1, "200", "only");

        REQUIRE(co_await server.pump([&] { return server.closed; }, 3s));
        CHECK(server.goaway);

        REQUIRE(co_await out.wait(sched));
        CHECK(out.body == "only");
    };

    sched.run_to_completion(test(&sched, &client, &host));
}

TEST_CASE("an HTTP/2 connection with a stream reserved isn't idle", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};

    const auto test = [](net::io::scheduler* sched) -> task<>
    {
        auto            fake = co_await connect_fake(sched, {.idle_timeout = 100ms});
        test::h2_client server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        // reserved, and not sent for longer than the idle timeout.
        REQUIRE(fake.conn->reserve());
        CHECK_FALSE(fake.conn->idle());
        CHECK_FALSE(fake.conn->idle_since().has_value());
        (void)co_await sched->sleep_until(net::io::clock::now() + 300ms);

        outcome out;
        REQUIRE(sched->spawn(send_get(sched, fake.conn, net::url::parse("http://127.0.0.1/").value(), &out)));
        REQUIRE(co_await server.pump([&] { return server.responses.contains(1); }));
        server.respond(1, "204");
        REQUIRE(co_await server.pump([] { return true; }));

        REQUIRE(co_await out.wait(sched));
        CHECK(out.status == 204);
        fake.conn->close();
    };

    sched.run_to_completion(test(&sched));
}

TEST_CASE("an HTTP/2 request refused before it's sent says so, and leaves its body unread", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};

    const auto test = [](net::io::scheduler* sched) -> task<>
    {
        auto            fake = co_await connect_fake(sched, {});
        test::h2_client server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        // a stream reserved, then the server's GOAWAY.
        REQUIRE(fake.conn->reserve());
        server.s.goaway(0, http2::error_code::no_error);
        REQUIRE(co_await server.sync());
        CHECK_FALSE(fake.conn->accepting());

        std::atomic<int>     reads{0};
        http::client_request req{.method = http::request_method::POST,
                                 .uri    = net::url::parse("http://127.0.0.1/").value()};
        req.body = std::make_unique<counting_reader>(100, &reads);

        http::detail::http2_client_connection::send_state state;
        const auto                                        resp = co_await fake.conn->send(req, &state);
        REQUIRE_FALSE(resp.has_value());
        CHECK(resp.error() == http2::error_code::refused_stream);
        CHECK(state.unsent);
        CHECK(reads.load() == 0);

        // and with nothing left to do, the connection ends.
        REQUIRE(co_await server.pump([&] { return server.closed; }, 3s));
    };

    sched.run_to_completion(test(&sched));
}
