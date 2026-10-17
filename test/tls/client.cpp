#include "http/client.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/connection_pool.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tls/test_pki.hpp"
#include "tls/tls.hpp"

namespace tls = net::tls;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::pki;
using test::server_with;

namespace
{

// https_server answers each request on its connections with "hello", over TLS, one connection at a time.
struct https_server
{
    net::io::scheduler* sched;
    net::listener       listener;
    tls::server_context ctx;
    std::atomic<int>    accepted{0};
    std::atomic<int>    requests{0};
    std::string         last_head;

    https_server(net::io::scheduler* sched, tls::server_context ctx)
        : sched{sched}
        , listener{net::listener::make(sched, "127.0.0.1", "0").value()}
        , ctx{std::move(ctx)}
    {
        REQUIRE(sched->spawn(serve(this)));
    }

    [[nodiscard]] std::string port() const { return std::to_string(listener.local_port()); }

    static task<> serve(https_server* self)
    {
        while (true)
        {
            auto conn = co_await self->listener.accept();
            if (!conn.has_value()) co_return;
            ++self->accepted;

            auto s = co_await tls::stream::accept(std::move(*conn), self->ctx, net::io::clock::now() + 5s);
            if (!s.has_value()) continue;

            std::string                  head;
            std::array<std::byte, 1'024> buf{};
            while (true)
            {
                auto res = co_await s->read(buf);
                head.append(reinterpret_cast<const char*>(buf.data()), res.count);
                if (res.err) break;

                if (head.contains("\r\n\r\n"))
                {
                    self->last_head = head.substr(0, head.find("\r\n\r\n") + 2);
                    head.clear();
                    ++self->requests;
                    co_await net::io::write_all(*s, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"sv);
                }
            }
        }
    }
};

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

net::http::client_options trusting_options()
{
    net::http::client_options options;
    options.tls.trusted_ca_file = pki().ca;
    return options;
}

// fetch GETs url, returning the body, or the error.
task<std::string> fetch(net::http::client* client, std::string url)
{
    auto resp = co_await client->get(url);
    if (!resp.has_value()) co_return "error: " + resp.error().message();

    auto body = co_await net::io::read_all(*resp->body);
    co_return body.has_value() ? *body : "error: " + body.error().message();
}

}

TEST_CASE("https requests are sent over TLS, and their connections reused", "[tls][http][client]")
{
    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, server_with(pki().localhost_chain, pki().localhost_key)};
    net::http::client  client{&sched, trusting_options()};

    const auto test = [](net::http::client* client, std::string port) -> task<std::pair<std::string, std::string>>
    {
        const auto url = "https://localhost:" + port + "/path";
        co_return std::pair{co_await fetch(client, url), co_await fetch(client, url)};
    };

    auto [first, second] = sched.run_to_completion(test(&client, server.port()));

    // the server's task has finished (with what it recorded) once the scheduler has shut down.
    sched.shutdown();

    CHECK(first == "hello");
    CHECK(second == "hello");
    CHECK(server.accepted == 1);
    CHECK(server.requests == 2);
    CHECK(server.last_head.starts_with("GET /path HTTP/1.1\r\n"));
    CHECK(server.last_head.contains("\r\nHost: localhost:" + server.port() + "\r\n"));
    CHECK(client.stats() == net::http::connection_stats{.open = 1, .idle = 1});
}

TEST_CASE("https servers' certificates are verified", "[tls][http][client]")
{
    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, server_with(pki().other_name_chain, pki().other_name_key)};
    net::http::client  client{&sched, trusting_options()};

    const auto test = [](net::http::client* client, std::string port) -> task<std::error_code>
    {
        auto resp = co_await client->get("https://localhost:" + port + "/");
        co_return resp.has_value() ? std::error_code{} : resp.error();
    };

    auto err = sched.run_to_completion(test(&client, server.port()));
    CAPTURE(err.message());
    CHECK(err == tls::alert::bad_certificate);
    CHECK(err.category() == tls::verification_category());
    CHECK(client.stats() == net::http::connection_stats{});
}

TEST_CASE("a TLS handshake that doesn't finish times out", "[tls][http][client]")
{
    net::io::scheduler sched{make_pool()};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // accepts the connection, but never answers the handshake.
    const auto silent = [](net::listener* listener, net::io::scheduler* sched) -> task<>
    {
        auto accepted = co_await listener->accept();
        while (!co_await sched->sleep_until(net::io::clock::now() + 10ms))
        {}
    };
    REQUIRE(sched.spawn(silent(&*listener, &sched)));

    auto options                  = trusting_options();
    options.tls_handshake_timeout = 200ms;
    net::http::client client{&sched, options};

    const auto test = [](net::http::client* client,
                         std::string        port) -> task<std::pair<std::error_code, net::io::clock::duration>>
    {
        const auto start = net::io::clock::now();
        auto       resp  = co_await client->get("https://localhost:" + port + "/");
        co_return std::pair{resp.has_value() ? std::error_code{} : resp.error(), net::io::clock::now() - start};
    };

    auto [err, elapsed] = sched.run_to_completion(test(&client, std::to_string(listener->local_port())));
    CAPTURE(err.message());
    CHECK(err == std::errc::timed_out);
    CHECK(elapsed < 5s);
}

TEST_CASE("https requests to a public server", "[tls][http][client][network]")
{
    net::io::scheduler sched{make_pool()};
    net::http::client  client{&sched};

    const auto test = [](net::http::client* client) -> task<std::string>
    {
        auto resp = co_await client->get("https://example.com/");
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        auto body = co_await net::io::read_all(*resp->body);
        if (!body.has_value()) co_return "error: " + body.error().message();

        co_return std::to_string(static_cast<int>(resp->status_code));
    };

    CHECK(sched.run_to_completion(test(&client)) == "200");
}
