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
#include <optional>
#include <span>
#include <stop_token>
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
#include "http/http2_config.hpp"
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
#include "util/defer.hpp"

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

// fetch GETs url (canceled by stop), returning the response's version and body, or the error.
task<std::string> fetch(http::client* client, std::string url, std::stop_token stop = {})
{
    auto resp = co_await client->get(url, std::move(stop));
    if (!resp.has_value()) co_return "error: " + resp.error().message();

    auto body = co_await net::io::read_all(*resp->body);
    if (!body.has_value()) co_return "error: " + body.error().message();
    co_return std::format("{} {}", resp->version.major, *body);
}

// fetches are GETs sent at once, each by a task of its own: what each came to (see fetch()), and how many have. They're
// declared before the scheduler, which waits for the tasks.
struct fetches
{
    explicit fetches(std::size_t n)
        : results(n)
    {}

    std::vector<std::string>   results;
    std::atomic<std::uint64_t> done{0};

    // start GETs url with client, once for each result, returning whether every task was spawned.
    bool start(net::io::scheduler* sched, http::client* client, const std::string& url)
    {
        for (auto& result : results)
        {
            const bool spawned = sched->spawn(
                [](net::io::scheduler* sched, http::client* client, std::string url, std::string* result, fetches* self)
                    -> task<>
                {
                    *result = co_await fetch(client, std::move(url));
                    self->done.fetch_add(1);
                    sched->notify_all(self->done);
                }(sched, client, url, &result, this));
            if (!spawned) return false;
        }
        return true;
    }

    // wait waits until they've all finished, returning whether they have (not if the scheduler's stopped first).
    task<bool> wait(net::io::scheduler* sched)
    {
        for (auto seen = done.load(); seen < results.size(); seen = done.load())
        {
            if (co_await sched->wait_on(done, seen) == std::errc::operation_canceled) co_return false;
        }
        co_return true;
    }
};

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

// accept accepts the client's next connection to host, returning the server the test plays on it (null if none came).
task<std::unique_ptr<played>> accept(fake_host* host, const http2::session_config& config = {})
{
    auto accepted = co_await host->listener.accept();
    if (!accepted.has_value()) co_return nullptr;
    co_return std::make_unique<played>(std::move(*accepted), config);
}

// server_saw is what a server the test played received (see test::h2_client), kept for the test to check once it's
// done.
struct server_saw
{
    std::map<http2::stream_id, test::h2_client::response>           responses;
    std::map<http2::stream_id, std::vector<std::vector<std::byte>>> blocks;

    void take(const test::h2_client& server)
    {
        responses = server.responses;
        blocks    = server.blocks;
    }
};

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

// connect_fake makes a fake_server, or says why it couldn't.
task<std::expected<fake_server, std::string>> connect_fake(net::io::scheduler*                       sched,
                                                           const http::detail::http2_client_options& options)
{
    auto listener = net::listener::make(sched, "127.0.0.1", "0").value();
    auto client   = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(listener.local_port()));
    if (!client.has_value()) co_return std::unexpected("not connected: " + client.error().message());
    auto accepted = co_await listener.accept();
    if (!accepted.has_value()) co_return std::unexpected("not accepted: " + accepted.error().message());

    auto conn = std::make_shared<http::detail::http2_client_connection>(
        sched,
        std::make_unique<http::detail::connection>(std::move(*client)),
        options,
        http::detail::http2_client_connection::callbacks{});
    if (!conn->start()) co_return std::unexpected("the connection didn't start");
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
    fetches            sent{20}; // (before the scheduler, which waits for the requests)
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};
    http::client       client{&sched, prior_knowledge()};

    struct seen
    {
        std::string first;
        std::string failed; // what went wrong, if anything
        std::size_t open = 0;
    };

    const auto test = [&] -> task<seen>
    {
        // the first opens the connection; the rest go at once.
        seen out;
        out.first = co_await fetch(&client, server.url("/slow"));
        if (!sent.start(&sched, &client, server.url("/slow")))
        {
            out.failed = "not spawned";
            co_return out;
        }

        // until they've all finished.
        const bool finished = co_await sent.wait(&sched);
        if (!finished) out.failed = "not all of them finished";
        out.open = client.stats().open;
        co_return out;
    };

    const auto out = sched.run_to_completion(test());
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());
    CHECK(out.first == "2 slow");
    for (const auto& result : sent.results) CHECK(result == "2 slow");
    CHECK(out.open == 1);
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
    outcome            out; // (before the scheduler, which waits for the requests)
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};

    std::error_code reset; // the first stream's, as the server saw it
    bool            closed = false;

    const auto test = [&] -> task<std::string>
    {
        // a response dropped before its end, then another request.
        const bool spawned = sched.spawn(
            [](net::io::scheduler* sched, http::client* client, fake_host* host, outcome* out) -> task<>
            {
                {
                    auto resp = co_await client->get(host->url("/big"));
                    if (!resp.has_value())
                    {
                        // (what's recorded is then the first's failure: the next isn't sent.)
                        co_await record(sched, std::move(resp), out);
                        co_return;
                    }
                    // dropped, unread.
                }
                co_await send_with(sched,
                                   client,
                                   request(http::request_method::GET, host->url("/")),
                                   out,
                                   &host->listener);
            }(&sched, &client, &host, &out));
        if (!spawned) co_return "not spawned";

        auto conn = co_await accept(&host);
        if (conn == nullptr) co_return "not accepted";
        auto& server = conn->server;

        const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
        if (!requested) co_return "no first request";
        server.respond(1, "200", "part of it", false);

        // the stream's reset (CANCEL), and the next request comes on the same connection.
        const bool was_reset = co_await server.pump([&] { return static_cast<bool>(server.responses[1].reset); });
        if (!was_reset) co_return "the first stream wasn't reset";
        reset = server.responses[1].reset;

        const bool again = co_await server.pump([&] { return server.responses.contains(3); });
        if (!again) co_return "no second request";
        server.respond(3, "200", "again");
        const bool written = co_await server.pump([] { return true; }); // (just written)
        if (!written) co_return "the second response wasn't written";

        const bool finished = co_await out.wait(&sched);
        if (!finished) co_return "the second request didn't finish";
        closed = server.closed;
        co_return server.failure;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(reset == http2::error_code::cancel);
    CHECK(out.status == 200);
    CHECK(out.body == "again");
    CHECK_FALSE(closed);
}

TEST_CASE("responses dropped partly read give the HTTP/2 connection's window back", "[http][http2][client][flow]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<std::string>
    {
        auto socket = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
        if (!socket.has_value()) co_return "error: " + socket.error().message();

        // a connection whose window is 4 streams' (the client's is 1 GiB), which 30 responses' unread data (at least a
        // frame of 16 KiB each) would use up.
        http::detail::http2_client_options options{.io_timeout = 5s};
        options.session.connection_window = 4 * 65'535;

        auto conn = std::make_shared<http::detail::http2_client_connection>(
            sched,
            std::make_unique<http::detail::connection>(std::move(*socket)),
            options,
            http::detail::http2_client_connection::callbacks{});
        if (!conn->start()) co_return "error: the connection didn't start";
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&conn] noexcept { conn->close(); }};

        const auto big = net::url::parse(std::format("http://127.0.0.1:{}/big", port)).value();
        for (int i = 0; i < 30; ++i)
        {
            http::client_request req{.method = http::request_method::GET, .uri = big};
            auto                 resp = co_await conn->send(req);
            if (!resp.has_value()) co_return std::format("error: response {}: {}", i, resp.error().message());

            std::array<std::byte, 1> one{};
            const auto               read = co_await resp->body->read(one);
            if (read.err) co_return std::format("error: response {}'s body: {}", i, read.err.message());
            // dropped, mostly unread.
        }

        // then one read to its end: at once, rather than a byte at a time (all the window that would be left).
        http::client_request req{.method = http::request_method::GET, .uri = big};
        auto                 resp = co_await conn->send(req);
        if (!resp.has_value()) co_return "error: the last response: " + resp.error().message();

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
            if (!resp.has_value())
                co_return std::vector{std::format("error: response {}: {}", i, resp.error().message())};
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
    CAPTURE(results);
    REQUIRE(results.size() == 25);
    for (std::size_t i = 0; i < 24; ++i) CHECK(results[i] == "read 4000000");
    CHECK(results.back() == "open 1");
}

TEST_CASE("at the server's stream limit, the client opens another connection, or waits",
          "[http][http2][client][limits]")
{
    const bool strict = GENERATE(false, true);
    CAPTURE(strict);

    // the held requests, and their handlers: how many have started. (Before the scheduler, which waits for them.)
    constexpr std::size_t n = 6;
    fetches               sent{n};
    test::stop_probe      probe;

    net::io::scheduler sched{make_pool()};
    test::gate         g{&sched};

    // /held is answered once the test opens the gate, so the requests hold their streams until it does.
    auto r = make_router(&sched);
    r.GET("/held",
          [&sched, &probe, &g](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              test::stop_probe::bump(&sched, probe.started);
              co_await g.pass();
              auto* body = co_await resp.send(http::status::OK, 4);
              co_await net::io::write_all(*body, "held"sv);
          });
    test_server server{&sched, std::move(r), {.http2_max_concurrent_streams = 2}};

    auto options                          = prior_knowledge();
    options.strict_max_concurrent_streams = strict;
    http::client client{&sched, options};

    struct seen
    {
        std::string first;
        std::string failed;          // what went wrong, if anything
        bool        started = false; // as many handlers as the client could start ran
        std::size_t open    = 0;
    };

    const auto test = [&] -> task<seen>
    {
        // the first makes the connection (and learns the limit).
        seen out;
        out.first = co_await fetch(&client, server.url("/"));

        // then 6 at once, whose handlers hold their streams: past the limit, the client opens other connections for
        // the rest (6 handlers start, two a connection), or, strict, has them wait for the first two (2 start).
        if (!sent.start(&sched, &client, server.url("/held"))) out.failed = "not spawned";
        out.started = co_await test::stop_probe::reach(&sched, probe.started, strict ? 2 : n);

        // whatever came of it, the handlers are let go, so the requests finish.
        for (std::size_t i = 0; i < n; ++i) g.open();
        const bool finished = co_await sent.wait(&sched);
        if (out.failed.empty() && !finished) out.failed = "not all of them finished";
        out.open = client.stats().open;
        co_return out;
    };

    const auto out = sched.run_to_completion(test());
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());
    CHECK(out.first == "2 hello over HTTP/2");
    CHECK(out.started);
    for (const auto& result : sent.results) CHECK(result == "2 held");

    if (strict) CHECK(out.open == 1);
    else CHECK(out.open > 1);
}

TEST_CASE("an idle HTTP/2 connection is closed after idle_timeout", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};

    auto options         = prior_knowledge();
    options.idle_timeout = 1s;
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

        // until it's closed (5 seconds at most).
        const auto deadline = net::io::clock::now() + 5s;
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
    outcome            out; // (before the scheduler, which waits for the request)
    net::io::scheduler sched{make_pool()};

    const bool refused = GENERATE(true, false);
    CAPTURE(refused);

    bool goaway = false;

    const auto test = [&] -> task<std::string>
    {
        // the connection's closed once it's idle: which it never is, if the stream's kept.
        http::detail::http2_client_options options{.idle_timeout = 200ms};
        if (!refused) options.response_header_timeout = 200ms;

        auto made = co_await connect_fake(&sched, options);
        if (!made.has_value()) co_return made.error();
        auto& fake = *made;
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        test::h2_client  server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        if (!sched.spawn(send_get(&sched, fake.conn, net::url::parse("http://127.0.0.1/").value(), &out)))
            co_return "not spawned";
        const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
        if (!requested) co_return "no request";

        // refused, or never answered.
        if (refused) server.s.reset(1, http2::error_code::refused_stream);

        const bool closed = co_await server.pump([&] { return server.closed; }, 3s);
        if (!closed) co_return "the connection wasn't closed";
        goaway = server.goaway;

        const bool finished = co_await out.wait(&sched);
        if (!finished) co_return "the request didn't finish";
        co_return server.failure;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(goaway);
    if (refused) CHECK(out.err == http2::error_code::refused_stream);
    else CHECK(out.err == std::errc::timed_out);
}

TEST_CASE("RST_STREAM(NO_ERROR) cuts a response short, unless it's complete", "[http][http2][client]")
{
    outcome            out; // (before the scheduler, which waits for the request)
    net::io::scheduler sched{make_pool()};

    SECTION("before the response's end")
    {
        const auto test = [&] -> task<std::string>
        {
            auto made = co_await connect_fake(&sched, {.io_timeout = 3s});
            if (!made.has_value()) co_return made.error();
            auto& fake = *made;
            // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
            net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
            test::h2_client  server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

            if (!sched.spawn(send_get(&sched, fake.conn, net::url::parse("http://127.0.0.1/").value(), &out)))
                co_return "not spawned";
            const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
            if (!requested) co_return "no request";

            const std::vector<net::http::hpack::field> head{
                {.name = ":status", .value = "200"}
            };
            if (const auto err = server.s.submit_headers(1, head, false)) co_return "head: " + err.message();
            const auto sent = server.s.submit_data(1, std::as_bytes(std::span{"part"sv}), false);
            if (sent != 4U) co_return "the body wasn't submitted";
            server.s.reset(1, http2::error_code::no_error);
            const bool written = co_await server.pump([] { return true; }); // (just written)
            if (!written) co_return "not written";

            const bool finished = co_await out.wait(&sched);
            if (!finished) co_return "the request didn't finish";
            co_return server.failure;
        };

        const auto failed = sched.run_to_completion(test());
        CAPTURE(failed);
        REQUIRE(failed.empty());
        CHECK(out.status == 200);
        CHECK(out.err == std::errc::connection_reset);
        CHECK(out.err == net::io::status_condition::closed);
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
    // what the request that waits came to, and whether it has. (Before the scheduler, which waits for it.)
    std::string                waiting;
    std::atomic<std::uint64_t> done{0};

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

    const auto test = [&] -> task<std::vector<std::string>>
    {
        // the connection, and its limit.
        std::vector<std::string> out{co_await fetch(&client, server.url("/"))};

        // a response whose head has arrived, and its body not yet; then a request, which waits for the stream.
        auto held = co_await client.get(server.url("/held"));
        if (!held.has_value())
        {
            out.push_back("error: " + held.error().message());
            co_return out;
        }

        const bool spawned = sched.spawn(
            [](net::io::scheduler*         sched,
               http::client*               client,
               std::string                 url,
               std::string*                result,
               std::atomic<std::uint64_t>* done) -> task<>
            {
                *result = co_await fetch(client, url);
                done->store(1);
                sched->notify_all(*done);
            }(&sched, &client, server.url("/"), &waiting, &done));
        if (!spawned)
        {
            g.open();
            out.emplace_back("not spawned");
            co_return out;
        }

        // NOTE: a moment for the request to start waiting (else it finds the stream free, and the test proves less,
        //       but still passes): nothing public shows a request waiting for a stream (the pool's stats count
        //       connections only).
        (void)co_await sched.sleep_until(net::io::clock::now() + 100ms);

        // the response ends, but isn't read (nor dropped) until the request that was waiting is done.
        g.open();
        while (done.load() == 0)
        {
            if (co_await sched.wait_on(done, 0) == std::errc::operation_canceled) break;
        }
        out.push_back(waiting);

        auto body = co_await net::io::read_all(*held->body);
        out.push_back(body.value_or("error"));
        co_return out;
    };

    const auto results = sched.run_to_completion(test());
    CHECK(results == std::vector<std::string>{"2 hello over HTTP/2", "2 hello over HTTP/2", "held"});
}

TEST_CASE("a refused HTTP/2 request is sent again, on another connection", "[http][http2][client][retry]")
{
    outcome            out; // (before the scheduler, which waits for the request)
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};

    const bool goaway = GENERATE(true, false);
    CAPTURE(goaway);

    // a retry on the wrong connection would wait for its head, which never comes.
    auto options                    = prior_knowledge();
    options.response_header_timeout = 2s;
    http::client client{&sched, options};

    bool more = false; // something more came on the first connection

    const auto test = [&] -> task<std::string>
    {
        const bool spawned = sched.spawn(
            send_with(&sched, &client, request(http::request_method::GET, host.url("/")), &out, &host.listener));
        if (!spawned) co_return "not spawned";

        auto first = co_await accept(&host);
        if (first == nullptr) co_return "the first connection wasn't accepted";
        const bool requested = co_await first->server.pump([&] { return first->server.responses.contains(1); });
        if (!requested) co_return "no request on the first connection";

        // not processed: after GOAWAY's last stream, or refused.
        if (goaway) first->server.s.goaway(0, http2::error_code::no_error);
        else first->server.s.reset(1, http2::error_code::refused_stream);
        const bool refusal = co_await first->server.pump([] { return true; }); // (just written)
        if (!refusal) co_return "the refusal wasn't written";

        auto second = co_await accept(&host);
        if (second == nullptr) co_return "the second connection wasn't accepted";
        const bool again = co_await second->server.pump([&] { return second->server.responses.contains(1); });
        if (!again) co_return "no request on the second connection";
        second->server.respond(1, "200", "again");
        const bool written = co_await second->server.pump([] { return true; });
        if (!written) co_return "the response wasn't written";

        const bool finished = co_await out.wait(&sched);
        if (!finished) co_return "the request didn't finish";

        // nothing more came on the first.
        if (!goaway)
        {
            const bool synced = co_await first->server.sync();
            if (!synced) co_return "the first connection's PING wasn't answered";
            more = first->server.responses.contains(3);
        }
        co_return second->server.failure;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(out.status == 200);
    CHECK(out.body == "again");
    CHECK_FALSE(more);
}

TEST_CASE("a request safe to repeat is sent again if its HTTP/2 connection drops before its response",
          "[http][http2][client][retry]")
{
    outcome            out; // (before the scheduler, which waits for the request)
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};

    const auto method = GENERATE(http::request_method::GET, http::request_method::POST);
    CAPTURE(method);

    const auto test = [&] -> task<std::string>
    {
        const bool spawned =
            sched.spawn(send_with(&sched, &client, request(method, host.url("/")), &out, &host.listener));
        if (!spawned) co_return "not spawned";

        // the connection drops, without GOAWAY, once the request's arrived.
        {
            auto first = co_await accept(&host);
            if (first == nullptr) co_return "the first connection wasn't accepted";
            const bool requested = co_await first->server.pump([&] { return first->server.responses.contains(1); });
            if (!requested) co_return "no request on the first connection";
        }

        // NOTE: kept until the response has been read: closing a socket with data unread (e.g. the client's SETTINGS
        //       acknowledgement) resets the connection, and Linux then drops what it hasn't sent yet.
        std::unique_ptr<played> second;
        if (method == http::request_method::GET)
        {
            second = co_await accept(&host);
            if (second == nullptr) co_return "the second connection wasn't accepted";
            const bool again = co_await second->server.pump([&] { return second->server.responses.contains(1); });
            if (!again) co_return "no request on the second connection";
            second->server.respond(1, "200", "again");
            const bool written = co_await second->server.pump([] { return true; });
            if (!written) co_return "the response wasn't written";
        }

        const bool finished = co_await out.wait(&sched);
        if (!finished) co_return "the request didn't finish";
        co_return second != nullptr ? second->server.failure : std::string{};
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());
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
}

TEST_CASE("HTTP/2 responses time out: their heads, and their bodies' stalls", "[http][http2][client][timeout]")
{
    outcome            out; // (before the scheduler, which waits for the request)
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};

    const bool head = GENERATE(true, false);
    CAPTURE(head);

    auto options                    = prior_knowledge();
    options.response_header_timeout = 300ms;
    options.io_timeout              = 300ms;
    http::client client{&sched, options};

    std::error_code reset; // the stream's, as the server saw it
    bool            closed = false;

    const auto test = [&] -> task<std::string>
    {
        const bool spawned =
            sched.spawn(send_with(&sched, &client, request(http::request_method::GET, host.url("/")), &out));
        if (!spawned) co_return "not spawned";

        auto conn = co_await accept(&host);
        if (conn == nullptr) co_return "not accepted";
        auto&      server    = conn->server;
        const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
        if (!requested) co_return "no request";
        if (!head) server.respond(1, "200", "part", false);

        // the client gives up on the stream (alone).
        const bool was_reset = co_await server.pump([&] { return static_cast<bool>(server.responses[1].reset); }, 3s);
        if (!was_reset) co_return "the stream wasn't reset";
        reset  = server.responses[1].reset;
        closed = server.closed;

        const bool finished = co_await out.wait(&sched);
        if (!finished) co_return "the request didn't finish";
        co_return server.failure;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(reset == http2::error_code::cancel);
    CHECK_FALSE(closed);
    CHECK(out.err == std::errc::timed_out);
    CHECK(out.status == (head ? 0 : 200));
}

TEST_CASE("an HTTP/2 connection whose writes stall fails, with its streams", "[http][http2][client][timeout]")
{
    // (before the scheduler, which waits for the requests.)
    outcome first;
    outcome second;

    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};

    // nothing but io_timeout limits the second request, waiting for its head.
    auto options                    = prior_knowledge();
    options.response_header_timeout = 0ms;
    options.io_timeout              = 300ms;
    http::client client{&sched, options};

    const auto test = [&] -> task<std::string>
    {
        // a body that never ends, and a request after it (not safe to repeat, so it isn't, on another connection).
        const auto endless_post = [](const std::string& url)
        {
            auto req = request(http::request_method::POST, url);
            req.body = std::make_unique<endless>();
            return req;
        };

        if (!sched.spawn(send_with(&sched, &client, endless_post(host.url("/endless")), &first)))
            co_return "the first wasn't spawned";

        // windows that don't stop the body: only the server not reading does.
        http2::session_config config;
        config.local.initial_window_size = 0x7fff'ffff;
        config.connection_window         = 0x7fff'ffff;

        auto conn = co_await accept(&host, config);
        if (conn == nullptr) co_return "not accepted";
        auto&      server    = conn->server;
        const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
        if (!requested) co_return "no first request";
        if (!sched.spawn(send_with(&sched, &client, request(http::request_method::POST, host.url("/after")), &second)))
            co_return "the second wasn't spawned";
        const bool after = co_await server.pump([&] { return server.responses.contains(3); });
        if (!after) co_return "no second request";

        // the server stops reading: the client's writer stalls, and fails the connection.
        const bool second_done = co_await second.wait(&sched);
        if (!second_done) co_return "the second request didn't finish";
        const bool first_done = co_await first.wait(&sched);
        if (!first_done) co_return "the first request didn't finish";
        co_return server.failure;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(second.err == std::errc::connection_aborted);
    CHECK(first.err);
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

        std::array<std::byte, 1> one{};
        const auto               first = co_await resp->body->read(one);
        if (first.err) co_return "error: the first byte: " + first.err.message();
        if (first.count != 1) co_return std::format("error: the first read read {} bytes", first.count);

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
    outcome            out; // (before the scheduler, which waits for the request)
    net::io::scheduler sched{make_pool()};

    struct test_case
    {
        std::string_view uri;
        std::string_view host; // the request's Host, if any
        std::string_view authority;
    };

    const auto tc = GENERATE(
        test_case{.uri = "http://example.com/", .host = "other.example:8080", .authority = "other.example:8080"},
        test_case{.uri = "http://example.com/", .host = " other.example:8080\t", .authority = "other.example:8080"},
        test_case{.uri = "http://example.com:80/", .host = "", .authority = "example.com"},
        test_case{.uri = "http://example.com:8080/", .host = "", .authority = "example.com:8080"},
        test_case{.uri = "https://example.com:443/", .host = "", .authority = "example.com"},
        test_case{.uri = "https://example.com:80/", .host = "", .authority = "example.com:80"},
        test_case{.uri = "http://[::1]:80/", .host = "", .authority = "[::1]"});
    CAPTURE(tc.uri, tc.host);

    server_saw got;

    const auto test = [&] -> task<std::string>
    {
        auto made = co_await connect_fake(&sched, {});
        if (!made.has_value()) co_return made.error();
        auto& fake = *made;
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        test::h2_client  server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        http::client_request req{.method = http::request_method::GET, .uri = net::url::parse(tc.uri).value()};
        if (!tc.host.empty()) req.headers.set("Host", tc.host);

        if (!sched.spawn(send_request(&sched, fake.conn, std::move(req), &out))) co_return "not spawned";
        const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
        got.take(server);
        if (!requested) co_return "no request";

        server.respond(1, "204");
        const bool written = co_await server.pump([] { return true; });
        if (!written) co_return "not written";
        const bool finished = co_await out.wait(&sched);
        if (!finished) co_return "the request didn't finish";
        co_return server.failure;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());

    const auto& r = got.responses[1];
    CHECK(test::header(r, ":authority") == tc.authority);
    CHECK(test::header(r, "host") == "(none)");
    CHECK(out.status == 204);
}

TEST_CASE("field values' whitespace around them is dropped over HTTP/2, both ways", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};

    // a handler that answers with values that work over HTTP/1.1 (whose parsers strip the whitespace), and says what
    // it received.
    auto r = make_router(&sched);
    r.GET("/padded",
          [](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              const auto received = std::format("[{}]", req.headers.get("X-Sent"sv).value_or("(none)"));
              resp.headers().set("X-Padded", "value \t");
              resp.headers().set("X-Inside", " a b ");
              auto* body = co_await resp.send(http::status::OK, received.size());
              co_await net::io::write_all(*body, received);
          });
    test_server  server{&sched, std::move(r)};
    http::client client{&sched, prior_knowledge()};

    const auto test = [](http::client* client, std::string url) -> task<std::string>
    {
        http::client_request req{.method = http::request_method::GET, .uri = net::url::parse(url).value()};
        req.headers.set("X-Sent", "\tsent ");

        auto resp = co_await client->send(std::move(req));
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        auto body = co_await net::io::read_all(*resp->body);
        if (!body.has_value()) co_return "error: " + body.error().message();
        co_return std::format("HTTP/{} {} [{}] [{}] {}",
                              resp->version.major,
                              static_cast<int>(resp->status_code),
                              resp->headers.get("X-Padded"sv).value_or("(none)"),
                              resp->headers.get("X-Inside"sv).value_or("(none)"),
                              *body);
    };

    CHECK(sched.run_to_completion(test(&client, server.url("/padded"))) == "HTTP/2 200 [value] [a b] [sent]");
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
    // (before the scheduler, which waits for the requests.)
    outcome held;
    outcome waiting;

    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};

    auto options                          = prior_knowledge();
    options.strict_max_concurrent_streams = true;
    options.connect_timeout               = 2s;
    http::client client{&sched, options};

    const auto test = [&] -> task<std::string>
    {
        // a connection of one stream, which a response holds.
        http2::session_config config;
        config.local.max_concurrent_streams = 1;

        if (!sched.spawn(send_with(&sched, &client, request(http::request_method::GET, host.url("/held")), &held)))
            co_return "the held request wasn't spawned";

        auto first = co_await accept(&host, config);
        if (first == nullptr) co_return "the first connection wasn't accepted";
        const bool requested = co_await first->server.pump([&] { return first->server.responses.contains(1); });
        if (!requested) co_return "no request on the first connection";
        first->server.respond(1, "200", "held", false);
        const bool synced = co_await first->server.sync();
        if (!synced) co_return "the first connection's PING wasn't answered";

        // a request waiting for its stream.
        const bool spawned = sched.spawn(
            send_with(&sched, &client, request(http::request_method::GET, host.url("/")), &waiting, &host.listener));
        if (!spawned) co_return "the waiting request wasn't spawned";

        // NOTE: a moment for the request to start waiting (else it finds the connection draining, and the test proves
        //       less, but still passes): nothing public shows a request waiting for a stream (the pool's stats count
        //       connections only).
        (void)co_await sched.sleep_until(net::io::clock::now() + 100ms);

        // the server's done with the connection, once its stream is: the request goes elsewhere.
        first->server.s.goaway(1, http2::error_code::no_error);
        const bool drained = co_await first->server.pump([] { return true; });
        if (!drained) co_return "the GOAWAY wasn't written";

        auto second = co_await accept(&host, config);
        if (second == nullptr) co_return "the second connection wasn't accepted";
        const bool again = co_await second->server.pump([&] { return second->server.responses.contains(1); });
        if (!again) co_return "no request on the second connection";
        second->server.respond(1, "200", "elsewhere");
        const bool written = co_await second->server.pump([] { return true; });
        if (!written) co_return "the second connection's response wasn't written";
        const bool answered = co_await waiting.wait(&sched);
        if (!answered) co_return "the waiting request didn't finish";

        // and the first stream finishes as it was.
        first->server.send(1, "", true);
        const bool ended = co_await first->server.pump([] { return true; });
        if (!ended) co_return "the held response's end wasn't written";
        const bool finished = co_await held.wait(&sched);
        if (!finished) co_return "the held request didn't finish";
        co_return !first->server.failure.empty() ? first->server.failure : second->server.failure;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(waiting.body == "elsewhere");
    CHECK(held.body == "held");
}

TEST_CASE("a burst of requests to an HTTP/2 host shares the connection being made", "[http][http2][client][limits]")
{
    fetches            sent{10}; // (before the scheduler, which waits for the requests)
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};
    http::client       client{&sched, prior_knowledge()};

    const auto test = [&] -> task<std::string>
    {
        // all at once, with no connection yet (prior knowledge says it speaks HTTP/2).
        if (!sent.start(&sched, &client, server.url("/"))) co_return "error: not spawned";
        const bool finished = co_await sent.wait(&sched);
        if (!finished) co_return "error: not all of them finished";
        co_return std::format("open {}", client.stats().open);
    };

    CHECK(sched.run_to_completion(test()) == "open 1");
    for (const auto& result : sent.results) CHECK(result == "2 hello over HTTP/2");
}

namespace
{

// connect_to connects pool to the server on port, over HTTP/2 (learning that the host speaks it), returning the
// connection, or nullptr if that failed.
task<std::shared_ptr<http::detail::http2_client_connection>> connect_to(http::detail::connection_pool* pool,
                                                                        std::string                    port)
{
    auto leased = co_await pool->acquire("http", "127.0.0.1", port, true);
    if (!leased.has_value()) co_return nullptr;
    co_return std::move(leased->http2);
}

}

TEST_CASE("the client's pool knows a host speaks HTTP/2 only while it has connections", "[http][http2][client][limits]")
{
    net::io::scheduler sched{make_pool()};
    test_server        a{&sched};
    test_server        b{&sched};

    // NOTE: with prior knowledge (cleartext), which it doesn't need to know them, but learns them all the same.
    auto pool = std::make_shared<http::detail::connection_pool>(&sched, prior_knowledge());

    const auto port = [](const test_server& s) { return std::to_string(s.server.local_port()); };

    // knows lists the servers the pool knows speak HTTP/2, and how many hosts it knows altogether.
    const auto knows = [&]
    {
        std::string out;
        for (const auto& [name, s] : {
                 std::pair{"a"sv, &a},
                 std::pair{"b"sv, &b}
        })
        {
            if (pool->knows_http2("http", "127.0.0.1", port(*s))) out += name;
        }
        return std::format("{} of {}", out.empty() ? "none" : out, pool->known_http2_hosts());
    };

    // drop closes a kept connection to the server on port, and waits for it to be gone.
    const auto drop = [&](std::shared_ptr<http::detail::http2_client_connection> conn, std::string on) -> task<bool>
    {
        conn->close();
        conn.reset();
        const auto deadline = net::io::clock::now() + 5s;
        while (pool->stats("127.0.0.1", on).open != 0 && net::io::clock::now() < deadline)
            (void)co_await sched.sleep_until(net::io::clock::now() + 5ms);
        co_return pool->stats("127.0.0.1", on).open == 0;
    };

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> out;

        // a's connection is kept, while b's comes and goes.
        auto conn_a = co_await connect_to(pool.get(), port(a));
        if (conn_a == nullptr)
        {
            out.emplace_back("not connected to a");
            co_return out;
        }
        out.push_back(knows());

        auto conn_b = co_await connect_to(pool.get(), port(b));
        if (conn_b == nullptr)
        {
            out.emplace_back("not connected to b");
            co_return out;
        }
        out.push_back(knows());
        const bool dropped_b = co_await drop(std::move(conn_b), port(b));
        if (!dropped_b)
        {
            out.emplace_back("b's connection wasn't dropped");
            co_return out;
        }
        out.push_back(knows());

        const bool dropped_a = co_await drop(std::move(conn_a), port(a));
        if (!dropped_a)
        {
            out.emplace_back("a's connection wasn't dropped");
            co_return out;
        }
        out.push_back(knows());

        // learned again with its next connection.
        auto conn = co_await connect_to(pool.get(), port(a));
        if (conn == nullptr)
        {
            out.emplace_back("not connected to a again");
            co_return out;
        }
        out.push_back(knows());
        conn->close();
        co_return out;
    };

    CHECK(sched.run_to_completion(test())
          == std::vector<std::string>{"a of 1", "ab of 2", "a of 1", "none of 0", "a of 1"});
}

TEST_CASE("with idle_timeout 0, an HTTP/2 connection closes once its last stream is done", "[http][http2][client]")
{
    outcome            out; // (before the scheduler, which waits for the request)
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};

    auto options         = prior_knowledge();
    options.idle_timeout = 0ms;
    http::client client{&sched, options};

    bool goaway = false;

    const auto test = [&] -> task<std::string>
    {
        const bool spawned =
            sched.spawn(send_with(&sched, &client, request(http::request_method::GET, host.url("/")), &out));
        if (!spawned) co_return "not spawned";

        auto conn = co_await accept(&host);
        if (conn == nullptr) co_return "not accepted";
        auto&      server    = conn->server;
        const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
        if (!requested) co_return "no request";
        server.respond(1, "200", "only");

        const bool closed = co_await server.pump([&] { return server.closed; }, 3s);
        if (!closed) co_return "the connection wasn't closed";
        goaway = server.goaway;

        const bool finished = co_await out.wait(&sched);
        if (!finished) co_return "the request didn't finish";
        co_return server.failure;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(goaway);
    CHECK(out.body == "only");
}

TEST_CASE("an HTTP/2 connection with a stream reserved isn't idle", "[http][http2][client]")
{
    outcome            out; // (before the scheduler, which waits for the request)
    net::io::scheduler sched{make_pool()};

    // whether the connection was idle (or had been since some time) once the stream was reserved.
    bool idle       = true;
    bool idle_since = true;

    const auto test = [&] -> task<std::string>
    {
        auto made = co_await connect_fake(&sched, {.idle_timeout = 100ms});
        if (!made.has_value()) co_return made.error();
        auto& fake = *made;
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        test::h2_client  server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        // reserved, and not sent for longer than the idle timeout.
        if (!fake.conn->reserve()) co_return "not reserved";
        idle       = fake.conn->idle();
        idle_since = fake.conn->idle_since().has_value();

        // NOTE: timed: nothing shows that the connection's idle check has run, but by 300 ms (three times the idle
        //       timeout) it would have closed a connection it took for idle, failing the request after.
        (void)co_await sched.sleep_until(net::io::clock::now() + 300ms);

        if (!sched.spawn(send_get(&sched, fake.conn, net::url::parse("http://127.0.0.1/").value(), &out)))
            co_return "not spawned";
        const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
        if (!requested) co_return "no request";
        server.respond(1, "204");
        const bool written = co_await server.pump([] { return true; });
        if (!written) co_return "not written";

        const bool finished = co_await out.wait(&sched);
        if (!finished) co_return "the request didn't finish";
        co_return server.failure;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK_FALSE(idle);
    CHECK_FALSE(idle_since);
    CHECK(out.status == 204);
}

TEST_CASE("an HTTP/2 request refused before it's sent says so, and leaves its body unread", "[http][http2][client]")
{
    net::io::scheduler sched{make_pool()};

    struct seen
    {
        std::string     failed;           // what went wrong, if anything
        bool            accepting = true; // the connection, after the server's GOAWAY
        std::error_code err;              // what the request came to
        bool            unsent = false;
        int             reads  = 0; // of its body
    };

    const auto test = [&] -> task<seen>
    {
        seen out;
        auto made = co_await connect_fake(&sched, {});
        if (!made.has_value())
        {
            out.failed = made.error();
            co_return out;
        }
        auto& fake = *made;
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        test::h2_client  server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        // a stream reserved, then the server's GOAWAY.
        if (!fake.conn->reserve())
        {
            out.failed = "not reserved";
            co_return out;
        }
        server.s.goaway(0, http2::error_code::no_error);
        const bool synced = co_await server.sync();
        if (!synced)
        {
            out.failed = "the PING wasn't answered";
            co_return out;
        }
        out.accepting = fake.conn->accepting();

        std::atomic<int>     reads{0};
        http::client_request req{.method = http::request_method::POST,
                                 .uri    = net::url::parse("http://127.0.0.1/").value()};
        req.body = std::make_unique<counting_reader>(100, &reads);

        http::detail::http2_client_connection::send_state state;
        const auto                                        resp = co_await fake.conn->send(req, &state);
        out.err                                                = resp.has_value() ? std::error_code{} : resp.error();
        out.unsent                                             = state.unsent;
        out.reads                                              = reads.load();

        // and with nothing left to do, the connection ends.
        const bool closed = co_await server.pump([&] { return server.closed; }, 3s);
        if (!closed) out.failed = "the connection didn't end";
        co_return out;
    };

    const auto out = sched.run_to_completion(test());
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());
    CHECK_FALSE(out.accepting);
    CHECK(out.err == http2::error_code::refused_stream);
    CHECK(out.unsent);
    CHECK(out.reads == 0);
}

namespace
{

std::error_code canceled() { return std::make_error_code(std::errc::operation_canceled); }

// stop_when requests stop once word reaches n (or after 5 seconds).
task<> stop_when(net::io::scheduler* sched, std::stop_source source, const std::atomic<std::uint64_t>* word, int n)
{
    (void)co_await test::stop_probe::reach(sched, *word, static_cast<std::uint64_t>(n));
    source.request_stop();
}

// cancel_router is make_router's, with handlers that wait on their request's token (see test::stop_probe):
// - /wait waits, then answers "waited" if it wasn't stopped;
// - /partial sends a head (with a Content-Length of 100) and some of its body, then waits.
http::router cancel_router(net::io::scheduler* sched, test::stop_probe* probe)
{
    auto r = make_router(sched);
    r.GET("/wait",
          [sched, probe](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              if (co_await probe->wait(sched, req.stop)) co_return;
              auto* body = co_await resp.send(http::status::OK, 6);
              co_await net::io::write_all(*body, "waited"sv);
          });
    r.GET("/partial",
          [sched, probe](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              auto* body = co_await resp.send(http::status::OK, 100);
              co_await net::io::write_all(*body, "partial"sv);
              (void)co_await probe->wait(sched, req.stop);
          });
    return r;
}

}

TEST_CASE("an HTTP/2 request canceled resets its stream, not the connection", "[http][http2][client][cancel]")
{
    enum class where : std::uint8_t
    {
        head,         // waiting for the response's head
        body,         // reading the response's body, after send() returned
        request_body, // sending the request's body, waiting for the stream's window
    };
    const auto at = GENERATE(where::head, where::body, where::request_body);
    CAPTURE(static_cast<int>(at));

    // (before the scheduler, which waits for the requests.)
    std::stop_source           source;
    std::atomic<std::uint64_t> head{0}; // the client has the response's head, and read the body's first part
    outcome                    out;
    outcome                    next;

    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};

    std::error_code reset; // the stream's, as the server saw it
    bool            closed = false;

    const auto test = [&] -> task<std::string>
    {
        auto req =
            request(at == where::request_body ? http::request_method::POST : http::request_method::GET, host.url("/"));
        req.stop = source.get_token();
        if (at == where::request_body) req.body = std::make_unique<endless>();

        auto sending = [](net::io::scheduler*         sched,
                          http::client*               client,
                          http::client_request        req,
                          outcome*                    out,
                          std::atomic<std::uint64_t>* head) -> task<>
        {
            auto resp = co_await client->send(std::move(req));
            if (resp.has_value())
            {
                std::array<std::byte, 16> buf{};
                const auto                first = co_await resp->body->read(buf);
                out->body.append(reinterpret_cast<const char*>(buf.data()), first.count);
                test::stop_probe::bump(sched, *head);
            }
            co_await record(sched, std::move(resp), out);
        }(&sched, &client, std::move(req), &out, &head);
        if (!sched.spawn(std::move(sending))) co_return "not spawned";

        auto conn = co_await accept(&host);
        if (conn == nullptr) co_return "not accepted";
        auto& server = conn->server;

        // (the request's body isn't consumed, so the client's held to the stream's window.)
        if (at == where::request_body) server.hold_windows = true;

        const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
        if (!requested) co_return "no request";

        if (at == where::body)
        {
            server.respond(1, "200", "part", false);
            const bool written = co_await server.pump([] { return true; }); // (just written)
            if (!written) co_return "the response wasn't written";
            const bool read = co_await test::stop_probe::reach(&sched, head, 1);
            if (!read) co_return "the client didn't read the body";
        }
        else if (at == where::request_body)
        {
            // once the stream's initial window of the body has come, the client's waiting for more.
            const bool windowed = co_await server.pump([&] { return server.responses[1].body.size() >= 65'535; });
            if (!windowed) co_return "the stream's window of the body didn't come";
        }

        source.request_stop();
        const bool was_reset = co_await server.pump([&] { return static_cast<bool>(server.responses[1].reset); });
        if (!was_reset) co_return "the stream wasn't reset";
        reset = server.responses[1].reset;

        const bool finished = co_await out.wait(&sched);
        if (!finished) co_return "the request didn't finish";

        // the connection's still there, for the next request.
        const bool spawned = sched.spawn(
            send_with(&sched, &client, request(http::request_method::GET, host.url("/")), &next, &host.listener));
        if (!spawned) co_return "the next request wasn't spawned";
        const bool again = co_await server.pump([&] { return server.responses.contains(3); });
        if (!again) co_return "no next request";
        server.respond(3, "200", "again");
        const bool written = co_await server.pump([] { return true; }); // (just written)
        if (!written) co_return "the next response wasn't written";

        const bool answered = co_await next.wait(&sched);
        if (!answered) co_return "the next request didn't finish";
        closed = server.closed;
        co_return server.failure;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(reset == http2::error_code::cancel);
    CHECK(out.err == canceled());
    if (at == where::body) CHECK(out.body == "part");
    CHECK(next.body == "again");
    CHECK_FALSE(closed);
}

TEST_CASE("an HTTP/2 request canceled stops its handler's token", "[http][http2][client][cancel]")
{
    // NOTE: before the scheduler, as the handlers use it.
    test::stop_probe probe;

    net::io::scheduler sched{make_pool()};
    test_server        server{&sched, cancel_router(&sched, &probe)};
    http::client       client{&sched, prior_knowledge()};

    const auto path = GENERATE("/wait"sv, "/partial"sv);
    CAPTURE(path);

    const auto test = [](net::io::scheduler* sched,
                         http::client*       client,
                         test_server*        server,
                         test::stop_probe*   probe,
                         std::string_view    path) -> task<std::vector<std::string>>
    {
        std::stop_source source;
        if (!sched->spawn(stop_when(sched, source, &probe->started, 1)))
            co_return std::vector<std::string>{"not spawned"};

        // canceled waiting for /wait's head, or reading /partial's body: at once, not when something times out.
        const auto               start = net::io::clock::now();
        std::vector<std::string> out{co_await fetch(client, server->url(path), source.get_token())};
        out.emplace_back(net::io::clock::now() - start < 5s ? "in time" : "too slow");

        const auto woke = co_await test::stop_probe::reach(sched, probe->woke, 1);
        out.emplace_back(woke && probe->canceled.load() == 1 ? "handler stopped" : "handler not stopped");

        // on the same connection, which is then idle (before the scheduler's stopped, which ends it).
        out.push_back(co_await fetch(client, server->url("/")));
        const auto stats = client->stats();
        out.push_back(std::format("{} open, {} idle", stats.open, stats.idle));
        co_return out;
    };

    const auto results = sched.run_to_completion(test(&sched, &client, &server, &probe, path));
    CHECK(results
          == std::vector<std::string>{"error: " + canceled().message(),
                                      "in time",
                                      "handler stopped",
                                      "2 hello over HTTP/2",
                                      "1 open, 1 idle"});
}

TEST_CASE("an HTTP/2 response's body isn't read once its request's canceled, though it's arrived",
          "[http][http2][client][cancel]")
{
    // the request, read by a task of its own: whether it had a response (why not, if it hadn't), what its two reads
    // came to, and words for its steps. (Before the scheduler, which waits for the task.)
    struct reading
    {
        std::stop_source           source;
        std::atomic<std::uint64_t> headed{0};   // the client has the response's head (or none came)
        std::atomic<std::uint64_t> stopped{0};  // the test has requested stop: the reads may go
        std::atomic<std::uint64_t> finished{0}; // the task's done
        std::string                error;
        std::vector<std::string>   reads;
    } r;

    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};

    const auto test = [&] -> task<std::string>
    {
        const bool spawned = sched.spawn(
            [](net::io::scheduler* sched, http::client* client, std::string url, reading* out) -> task<>
            {
                auto resp = co_await client->get(url, out->source.get_token());
                if (!resp.has_value()) out->error = resp.error().message();
                test::stop_probe::bump(sched, out->headed);

                if (resp.has_value())
                {
                    (void)co_await test::stop_probe::reach(sched, out->stopped, 1);
                    std::array<std::byte, 64> buf{};
                    for (int i = 0; i < 2; ++i)
                    {
                        const auto res = co_await resp->body->read(buf);
                        out->reads.push_back(std::format("{} {}", res.count, res.err.message()));
                    }
                }
                test::stop_probe::bump(sched, out->finished);
            }(&sched, &client, host.url("/"), &r));
        if (!spawned) co_return "not spawned";

        auto conn = co_await accept(&host);
        if (conn == nullptr) co_return "not accepted";
        auto&      server    = conn->server;
        const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
        if (!requested) co_return "no request";

        // the whole response; then a PING, whose acknowledgement says the client has taken all of it in.
        server.respond(1, "200", "hello");
        const bool synced = co_await server.sync();
        if (!synced) co_return "the PING wasn't answered";
        const bool headed = co_await test::stop_probe::reach(&sched, r.headed, 1);
        if (!headed) co_return "the client didn't get the head";

        // canceled, then read.
        r.source.request_stop();
        test::stop_probe::bump(&sched, r.stopped);
        const bool finished = co_await test::stop_probe::reach(&sched, r.finished, 1);
        if (!finished) co_return "the reads didn't end";
        co_return server.failure;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CAPTURE(r.error);
    const auto message = canceled().message();
    CHECK(r.reads == std::vector<std::string>{"0 " + message, "0 " + message});
}

TEST_CASE("an HTTP/2 request canceled while it waits for a stream gives up its place", "[http][http2][client][cancel]")
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

    auto options                          = prior_knowledge();
    options.strict_max_concurrent_streams = true;
    options.connect_timeout               = 10s;
    http::client client{&sched, options};

    const auto test = [](net::io::scheduler* sched, http::client* client, test::gate* g, test_server* server)
        -> task<std::vector<std::string>>
    {
        // the connection, and its limit.
        std::vector<std::string> out{co_await fetch(client, server->url("/"))};

        // the one stream's busy, so the next request waits for it, until it's canceled.
        auto held = co_await client->get(server->url("/held"));
        if (!held.has_value())
        {
            out.push_back("error: " + held.error().message());
            co_return out;
        }

        // NOTE: stopped after a moment, for the request to be waiting by then (else it's stopped before, and the test
        //       proves less, but still passes): nothing public shows a request waiting for a stream (the pool's stats
        //       count connections only).
        std::stop_source source;
        const bool       spawned = sched->spawn(
            [](net::io::scheduler* sched, std::stop_source source) -> task<>
            {
                (void)co_await sched->sleep_until(net::io::clock::now() + 100ms);
                source.request_stop();
            }(sched, source));
        if (!spawned)
        {
            g->open();
            out.emplace_back("not spawned");
            co_return out;
        }
        const auto start = net::io::clock::now();
        out.push_back(co_await fetch(client, server->url("/"), source.get_token()));
        out.emplace_back(net::io::clock::now() - start < 5s ? "in time" : "too slow");

        g->open();
        auto body = co_await net::io::read_all(*held->body);
        out.push_back(body.value_or("error"));
        held = std::unexpected(std::error_code{}); // (done with)

        // on the same connection, which is then idle (before the scheduler's stopped, which ends it): nothing's
        // left reserved.
        out.push_back(co_await fetch(client, server->url("/")));
        const auto stats = client->stats();
        out.push_back(std::format("{} open, {} idle", stats.open, stats.idle));
        co_return out;
    };

    const auto results = sched.run_to_completion(test(&sched, &client, &g, &server));
    CHECK(results
          == std::vector<std::string>{"2 hello over HTTP/2",
                                      "error: " + canceled().message(),
                                      "in time",
                                      "held",
                                      "2 hello over HTTP/2",
                                      "1 open, 1 idle"});
}

TEST_CASE("an HTTP/2 request stopped before its stream's opened sends nothing, and gives its stream back",
          "[http][http2][client][cancel]")
{
    net::io::scheduler sched{make_pool()};

    struct seen
    {
        std::string     failed;      // what went wrong, if anything
        bool            idle = true; // the connection, with the stream reserved
        std::error_code err;         // what the request came to
        bool            idle_after = false;
        bool            got        = true; // the server got something of it
    };

    const auto test = [&] -> task<seen>
    {
        seen out;
        auto made = co_await connect_fake(&sched, {});
        if (!made.has_value())
        {
            out.failed = made.error();
            co_return out;
        }
        auto& fake = *made;
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        test::h2_client  server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        // as the pool does: a stream reserved for the request, which send() takes.
        if (!fake.conn->reserve())
        {
            out.failed = "not reserved";
            co_return out;
        }
        out.idle = fake.conn->idle();

        std::stop_source source;
        source.request_stop();
        auto req = request(http::request_method::GET, "http://127.0.0.1/");
        req.stop = source.get_token();

        auto resp      = co_await fake.conn->send(req);
        out.err        = resp.has_value() ? std::error_code{} : resp.error();
        out.idle_after = fake.conn->idle();

        // the server got nothing of it.
        const auto synced = co_await server.sync();
        if (!synced)
        {
            out.failed = "the PING wasn't answered";
            co_return out;
        }
        out.got = !server.responses.empty();
        co_return out;
    };

    const auto out = sched.run_to_completion(test());
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());
    CHECK_FALSE(out.idle);
    CHECK(out.err == canceled());
    CHECK(out.idle_after);
    CHECK_FALSE(out.got);
}

TEST_CASE("an HTTP/2 request racing its cancellation ends once, and frees its stream", "[http][http2][client][cancel]")
{
    net::io::scheduler sched{make_pool()};
    test_server        server{&sched};
    http::client       client{&sched, prior_knowledge()};

    const auto test =
        [](net::io::scheduler* sched, http::client* client, test_server* server) -> task<std::vector<std::string>>
    {
        std::vector<std::string> unexpected;
        const auto               url = server->url("/");

        for (int round = 0; round < 300; ++round)
        {
            // stop before, during, or after the request (the delays straddle a round trip).
            std::stop_source source;
            const auto       delay = std::chrono::microseconds{(round % 10) * 30};
            if (round % 10 == 0)
            {
                source.request_stop();
            }
            else if (!sched->spawn(
                         [](net::io::scheduler* sched, std::stop_source source, std::chrono::microseconds delay)
                             -> task<>
                         {
                             (void)co_await sched->sleep_until(net::io::clock::now() + delay);
                             source.request_stop();
                         }(sched, source, delay)))
            {
                co_return std::vector<std::string>{"stopped"};
            }

            auto result = co_await fetch(client, url, source.get_token());
            if (result != "2 hello over HTTP/2" && result != "error: " + canceled().message())
                unexpected.push_back(result);

            // whatever happened, the stream's given back: the connection's idle (and the only one).
            // (none, if the first requests were canceled as they connected.)
            const auto stats = client->stats();
            if (stats.open != stats.idle || stats.open > 1)
                unexpected.push_back(std::format("{} open, {} idle", stats.open, stats.idle));
        }
        co_return unexpected;
    };

    const auto unexpected = sched.run_to_completion(test(&sched, &client, &server));
    for (const auto& u : unexpected) UNSCOPED_INFO(u);
    CHECK(unexpected.empty());
}

TEST_CASE("a handler's outgoing requests given its token are canceled with it", "[http][http2][client][cancel]")
{
    // NOTE: before the scheduler, as the handlers use them.
    test::stop_probe probe; // the upstream's handlers

    struct proxied
    {
        std::atomic<std::uint64_t> done{0};
        std::error_code            err; // what the outgoing request came to
    } proxy;

    net::io::scheduler sched{make_pool()};

    // the upstream: its handler waits until it's stopped.
    test_server upstream{&sched, cancel_router(&sched, &probe)};

    // the server in front: its handler sends the request on, with its token, over HTTP/2 or HTTP/1.1.
    const bool http2_upstream = GENERATE(true, false);
    CAPTURE(http2_upstream);

    auto options  = prior_knowledge();
    options.http2 = http2_upstream;
    http::client outgoing{&sched, options};

    http::router r;
    r.GET("/proxy",
          [&](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              auto out = request(http::request_method::GET, upstream.url("/wait"));
              out.stop = req.stop;

              auto answer = co_await outgoing.send(std::move(out));
              proxy.err   = answer.has_value() ? std::error_code{} : answer.error();
              test::stop_probe::bump(&sched, proxy.done);

              if (answer.has_value()) (void)co_await resp.send(answer->status_code, 0);
          });
    test_server front{&sched, std::move(r)};

    http::client client{&sched, prior_knowledge()};

    const auto test = [](net::io::scheduler* sched,
                         http::client*       client,
                         test_server*        front,
                         test::stop_probe*   probe,
                         proxied*            proxy,
                         bool                http2_upstream) -> task<std::vector<std::string>>
    {
        // the client gives up once the upstream's handler is waiting: resetting its stream to the server in front,
        // whose handler's token stops, canceling its request.
        std::stop_source source;
        if (!sched->spawn(stop_when(sched, source, &probe->started, 1)))
            co_return std::vector<std::string>{"not spawned"};
        const auto               start = net::io::clock::now();
        std::vector<std::string> out{co_await fetch(client, front->url("/proxy"), source.get_token())};

        // at once, not when something times out.
        const auto proxied = co_await test::stop_probe::reach(sched, proxy->done, 1);
        out.push_back(proxied ? "outgoing: " + proxy->err.message() : "outgoing not done");
        out.emplace_back(net::io::clock::now() - start < 5s ? "in time" : "too slow");

        // over HTTP/2, the upstream's handler is stopped too (its stream's reset); over HTTP/1.1, the connection's
        // closed, which its server doesn't notice while the handler runs.
        if (http2_upstream)
        {
            const auto woke = co_await test::stop_probe::reach(sched, probe->woke, 1);
            out.emplace_back(woke && probe->canceled.load() == 1 ? "upstream stopped" : "upstream not stopped");
        }
        co_return out;
    };

    auto expected =
        std::vector<std::string>{"error: " + canceled().message(), "outgoing: " + canceled().message(), "in time"};
    if (http2_upstream) expected.emplace_back("upstream stopped");
    CHECK(sched.run_to_completion(test(&sched, &client, &front, &probe, &proxy, http2_upstream)) == expected);
}

namespace
{

// exchange sends req on fake's connection (recording what happens in out, which must outlive the scheduler, in case
// a test fails before the request's done), and answers it (stream id) as server with a 204, returning what went wrong
// (nothing, if the response came back as a 204).
task<std::string> exchange(net::io::scheduler*  sched,
                           const fake_server&   fake,
                           test::h2_client&     server,
                           http::client_request req,
                           http2::stream_id     id,
                           outcome*             out)
{
    if (!sched->spawn(send_request(sched, fake.conn, std::move(req), out)))
        co_return std::format("stream {}'s request wasn't spawned", id);
    const bool received = co_await server.pump([&] { return server.responses[id].done; });
    if (!received) co_return std::format("stream {}'s request didn't come", id);

    server.respond(id, "204");
    const bool written = co_await server.pump([] { return true; }); // (just written)
    if (!written) co_return std::format("stream {}'s response wasn't written", id);

    const bool answered = co_await out->wait(sched);
    if (!answered) co_return std::format("stream {}'s request didn't finish", id);
    if (out->status != 204)
        co_return std::format("stream {} came back as {} ({})", id, out->status, out->err.message());
    co_return server.failure;
}

}

TEST_CASE("the client's HTTP/2 requests repeat fields as indexes of HPACK's table, but never credentials",
          "[http][http2][client][hpack]")
{
    std::array<outcome, 3> outcomes; // (before the scheduler, which waits for the requests)
    net::io::scheduler     sched{make_pool()};

    const std::array<http2::stream_id, 3> ids{1, 3, 5};
    server_saw                            got;

    const auto test = [&] -> task<std::string>
    {
        auto made = co_await connect_fake(&sched, {.io_timeout = 3s});
        if (!made.has_value()) co_return made.error();
        auto& fake = *made;
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        test::h2_client  server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        const auto make = []
        {
            http::client_request req{.method = http::request_method::GET,
                                     .uri    = net::url::parse("http://example.com/some/path").value()};
            req.headers.set("User-Agent", "cppnet's tests")
                .set("Accept", "text/plain")
                .set("Authorization", "Bearer a-token-of-some-length")
                .set("Cookie", "id=42");
            return req;
        };

        for (const auto id : ids)
        {
            auto failed = co_await exchange(&sched, fake, server, make(), id, &outcomes.at(id / 2));
            if (!failed.empty()) co_return failed;
        }
        got.take(server);
        co_return std::string{};
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());

    for (const auto id : ids)
    {
        CAPTURE(id);
        const auto& r = got.responses[id];
        CHECK(test::header(r, ":authority") == "example.com");
        CHECK(test::header(r, ":path") == "/some/path");
        CHECK(test::header(r, "user-agent") == "cppnet's tests");
        CHECK(test::header(r, "accept") == "text/plain");
        CHECK(test::header(r, "authorization") == "Bearer a-token-of-some-length");
        CHECK(test::header(r, "cookie") == "id=42");
        CHECK(r.never_indexed == std::vector<std::string>{"authorization", "cookie"});
        REQUIRE(got.blocks[id].size() == 1);
        CHECK(test::size_updates(got.blocks[id][0]).empty());
    }

    // :authority, user-agent and accept are indexes after the first request (:path stays a literal, as it changes
    // from one request to the next, and so do the credentials, each time), so the others' blocks are smaller,
    // and the same.
    const auto first = got.blocks[1][0].size();
    CAPTURE(first, got.blocks[3][0].size());
    CHECK(got.blocks[3][0].size() + 20 < first);
    CHECK(got.blocks[3][0] == got.blocks[5][0]);
}

TEST_CASE("a server's SETTINGS_HEADER_TABLE_SIZE above 4096 leaves the client's HPACK table at 4096",
          "[http][http2][client][hpack]")
{
    std::array<outcome, 3> outcomes; // (before the scheduler, which waits for the requests)
    net::io::scheduler     sched{make_pool()};

    // x-a, then x-b, then x-a again: in a table of 4096, x-b pushes x-a out, so it's sent again as a literal (of
    // about 1900 bytes, Huffman coded); in one of 65536, it would be an index.
    const std::array<std::pair<http2::stream_id, char>, 3> sent{
        {{1, 'a'}, {3, 'b'}, {5, 'a'}}
    };
    server_saw got;

    const auto test = [&] -> task<std::string>
    {
        http2::session_config config;
        config.local.header_table_size = 65'536;

        auto made = co_await connect_fake(&sched, {.io_timeout = 3s});
        if (!made.has_value()) co_return made.error();
        auto& fake = *made;
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        test::h2_client  server{&fake.socket, &fake.socket, &fake.socket, http2::role::server, config};

        const auto with = [](char name)
        {
            http::client_request req{.method = http::request_method::GET,
                                     .uri    = net::url::parse("http://example.com/").value()};
            req.headers.set(std::format("X-{}", name), std::string(3'000, name));
            return req;
        };

        for (const auto& [id, name] : sent)
        {
            auto failed = co_await exchange(&sched, fake, server, with(name), id, &outcomes.at(id / 2));
            if (!failed.empty()) co_return failed;
        }
        got.take(server);
        co_return std::string{};
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());

    for (const auto& [id, name] : sent)
    {
        CAPTURE(id);
        CHECK(test::header(got.responses[id], std::format("x-{}", name)) == std::string(3'000, name));
        REQUIRE(got.blocks[id].size() == 1);
        CHECK(test::size_updates(got.blocks[id][0]).empty()); // still the 4096 the server knows
    }
    CHECK(got.blocks[5][0].size() > 1'000);
}

TEST_CASE("a server's SETTINGS_HEADER_TABLE_SIZE lowered then raised: the client's next block signals both",
          "[http][http2][client][hpack]")
{
    std::array<outcome, 3> outcomes; // (before the scheduler, which waits for the requests)
    net::io::scheduler     sched{make_pool()};

    server_saw got;

    const auto test = [&] -> task<std::string>
    {
        auto made = co_await connect_fake(&sched, {.io_timeout = 3s});
        if (!made.has_value()) co_return made.error();
        auto& fake = *made;
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        test::h2_client  server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        const auto make = []
        {
            http::client_request req{.method = http::request_method::GET,
                                     .uri    = net::url::parse("http://example.com/").value()};
            req.headers.set("User-Agent", "cppnet's tests").set("Accept", "text/plain");
            return req;
        };

        auto failed = co_await exchange(&sched, fake, server, make(), 1, &outcomes.at(0));
        if (!failed.empty()) co_return failed;

        // SETTINGS lowering the table to 100, then raising it past the cap, written by hand (the session can't change
        // its settings): the client's next block must start with 100, then its cap, 4096 (RFC 7541, section 4.2). The
        // PING's acknowledgement says the client has taken them, before the request's sent.
        std::vector<std::byte> frames;
        http2::encode_settings({.header_table_size = 100}, {}, frames);
        http2::encode_settings({.header_table_size = 8'192}, {}, frames);
        const auto written = co_await net::io::write_all(fake.socket, frames);
        if (written.err) co_return "the SETTINGS weren't written: " + written.err.message();
        const bool synced = co_await server.sync();
        if (!synced) co_return "the PING wasn't answered";

        failed = co_await exchange(&sched, fake, server, make(), 3, &outcomes.at(1));
        if (!failed.empty()) co_return failed;
        failed = co_await exchange(&sched, fake, server, make(), 5, &outcomes.at(2));
        if (!failed.empty()) co_return failed;
        got.take(server);
        co_return std::string{};
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());

    for (const http2::stream_id id : {1U, 3U, 5U})
    {
        CAPTURE(id);
        CHECK(test::header(got.responses[id], "user-agent") == "cppnet's tests");
        REQUIRE(got.blocks[id].size() == 1);
    }
    CHECK(test::size_updates(got.blocks[1][0]).empty());
    CHECK(test::size_updates(got.blocks[3][0]) == "3f453fe11f"); // 100, then 4096
    CHECK(test::size_updates(got.blocks[5][0]).empty());

    // the table was cut to 100, keeping at most one field, so the second request indexed the others again, and
    // the third has them as indexes.
    CHECK(got.blocks[5][0].size() + 10 < got.blocks[3][0].size());
}

TEST_CASE("an HTTP/2 request's body of known length ends with its last DATA frame, unless trailers follow",
          "[http][http2][client][end-stream]")
{
    enum class body_kind : std::uint8_t
    {
        length,  // with a Content-Length
        unknown, // without one
        none,    // no body
    };

    struct test_case
    {
        std::string_view           name;
        body_kind                  kind;
        std::size_t                size;
        bool                       trailers;
        int                        frames;        // DATA frames, empty ones too
        std::optional<std::size_t> ended_by_data; // the DATA frame that ended the stream, if one did: its length
        bool                       head_ended = false;
    };

    // (the client reads a body 16 KiB at a time, and sends what it reads as a frame of at most 16384 bytes.)
    const auto tc = GENERATE(test_case{.name          = "a Content-Length",
                                       .kind          = body_kind::length,
                                       .size          = 5,
                                       .trailers      = false,
                                       .frames        = 1,
                                       .ended_by_data = 5},
                             test_case{.name          = "a Content-Length of several frames",
                                       .kind          = body_kind::length,
                                       .size          = 40'000,
                                       .trailers      = false,
                                       .frames        = 3,
                                       .ended_by_data = 40'000 - (2 * 16'384)},
                             test_case{.name          = "a Content-Length and make_trailers",
                                       .kind          = body_kind::length,
                                       .size          = 5,
                                       .trailers      = true,
                                       .frames        = 1,
                                       .ended_by_data = std::nullopt},
                             test_case{.name          = "an unknown length",
                                       .kind          = body_kind::unknown,
                                       .size          = 5,
                                       .trailers      = false,
                                       .frames        = 2,
                                       .ended_by_data = 0},
                             test_case{.name          = "a Content-Length of 0",
                                       .kind          = body_kind::length,
                                       .size          = 0,
                                       .trailers      = false,
                                       .frames        = 0,
                                       .ended_by_data = std::nullopt,
                                       .head_ended    = true},
                             test_case{.name          = "make_trailers without a body",
                                       .kind          = body_kind::none,
                                       .size          = 0,
                                       .trailers      = true,
                                       .frames        = 0,
                                       .ended_by_data = std::nullopt});
    CAPTURE(tc.name);

    outcome            out; // (before the scheduler, which waits for the request)
    net::io::scheduler sched{make_pool()};

    const std::string body(tc.size, 'x');
    server_saw        got;

    const auto test = [&] -> task<std::string>
    {
        auto made = co_await connect_fake(&sched, {.io_timeout = 3s});
        if (!made.has_value()) co_return made.error();
        auto& fake = *made;
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        test::h2_client  server{&fake.socket, &fake.socket, &fake.socket, http2::role::server};

        http::client_request req{.method = http::request_method::POST,
                                 .uri    = net::url::parse("http://example.com/upload").value()};
        if (tc.kind == body_kind::length) req.set_body(body);
        else if (tc.kind == body_kind::unknown) req.body = std::make_unique<net::io::owned_string_reader>(body);
        if (tc.trailers) req.make_trailers = [](http::trailer_fields& t) { t.set("X-Checksum", "abc"); };

        auto failed = co_await exchange(&sched, fake, server, std::move(req), 1, &out);
        got.take(server);
        co_return failed;
    };

    const auto failed = sched.run_to_completion(test());
    CAPTURE(failed);
    REQUIRE(failed.empty());

    const auto& r = got.responses[1];
    CHECK(r.body == body);
    CHECK(r.head_ended == tc.head_ended);
    CHECK(r.data_frames == tc.frames);
    CHECK(r.ended_by_data == tc.ended_by_data);
    CHECK(test::trailer(r, "x-checksum") == (tc.trailers ? "abc" : "(none)"));
}
