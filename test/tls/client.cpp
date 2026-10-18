#include "http/client.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/connection_pool.hpp"
#include "http/request.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "tls/test_pki.hpp"
#include "tls/tls.hpp"
#include "url.hpp"

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

// scripted_server answers requests over TLS as respond says, one connection at a time: respond is given the stream,
// and the request's number (from 0, over all of the connections), and returns whether to keep reading the connection.
template<typename Respond>
struct scripted_server
{
    net::io::scheduler* sched;
    net::listener       listener;
    Respond             respond;
    std::atomic<int>    accepted{0};
    std::atomic<int>    requests{0};

    scripted_server(net::io::scheduler* sched, Respond respond)
        : sched{sched}
        , listener{net::listener::make(sched, "127.0.0.1", "0").value()}
        , respond{std::move(respond)}
    {
        REQUIRE(sched->spawn(serve(this)));
    }

    [[nodiscard]] std::string port() const { return std::to_string(listener.local_port()); }

    static task<> serve(scripted_server* self)
    {
        while (true)
        {
            auto conn = co_await self->listener.accept();
            if (!conn.has_value()) co_return;
            ++self->accepted;

            auto s = co_await tls::stream::accept(std::move(*conn),
                                                  server_with(pki().localhost_chain, pki().localhost_key),
                                                  net::io::clock::now() + 5s);
            if (!s.has_value()) continue;

            std::string                  head;
            std::array<std::byte, 1'024> buf{};
            bool                         more = true;
            while (more)
            {
                auto res = co_await s->read(buf);
                head.append(reinterpret_cast<const char*>(buf.data()), res.count);
                if (res.err) break;

                // NOTE: the requests have no bodies.
                while (more && head.contains("\r\n\r\n"))
                {
                    head.erase(0, head.find("\r\n\r\n") + 4);
                    more = co_await self->respond(*s, self->requests++);
                }
            }

            co_await s->close();
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

TEST_CASE("a request canceled during its TLS handshake fails, and counts its connection out",
          "[tls][http][client][cancel]")
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

    net::http::client client{&sched, trusting_options()};

    const auto test = [](net::io::scheduler* sched,
                         net::http::client*  client,
                         std::string         port) -> task<std::pair<std::error_code, net::io::clock::duration>>
    {
        std::stop_source source;
        REQUIRE(sched->spawn(
            [](net::io::scheduler* sched, std::stop_source source) -> task<>
            {
                (void)co_await sched->sleep_until(net::io::clock::now() + 100ms);
                source.request_stop();
            }(sched, source)));

        const auto start = net::io::clock::now();
        auto       resp  = co_await client->get("https://localhost:" + port + "/", source.get_token());
        co_return std::pair{resp.has_value() ? std::error_code{} : resp.error(), net::io::clock::now() - start};
    };

    auto [err, elapsed] = sched.run_to_completion(test(&sched, &client, std::to_string(listener->local_port())));
    CAPTURE(err.message());
    CHECK(err == std::errc::operation_canceled);
    CHECK(elapsed >= 100ms);
    CHECK(elapsed < 5s); // well before tls_handshake_timeout
    CHECK(client.stats() == net::http::connection_stats{});
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

TEST_CASE("a response the server sends after another's body isn't taken as the next request's", "[tls][http][client]")
{
    net::io::scheduler sched{make_pool()};

    // the first response (2 KiB, twice the client's buffer, so its body's end is read past the buffer, straight from
    // the stream) is followed by another, in the same write (and TLS record), which the client mustn't take as its next
    // request's.
    const auto respond = [](tls::stream& s, int n) -> task<bool>
    {
        std::string out;
        if (n == 0)
        {
            constexpr auto head = "HTTP/1.1 200 OK\r\nContent-Length: 0000\r\n\r\n"sv;
            const auto     body = std::string(2'048 - head.size(), 'a');
            out                 = std::format("HTTP/1.1 200 OK\r\nContent-Length: {:04}\r\n\r\n{}", body.size(), body)
                                + "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nINJECT";
        }
        else
        {
            out = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nreal";
        }

        auto res = co_await net::io::write_all(s, std::string_view{out});
        co_return !res.err;
    };
    scripted_server   server{&sched, respond};
    net::http::client client{&sched, trusting_options()};

    const auto test = [](net::http::client* client, std::string url) -> task<std::pair<std::string, std::string>>
    {
        auto first = co_await fetch(client, url);
        co_return std::pair{first, co_await fetch(client, url)};
    };

    auto [first, second] = sched.run_to_completion(test(&client, "https://localhost:" + server.port() + "/"));
    CHECK(first == std::string(2'048 - "HTTP/1.1 200 OK\r\nContent-Length: 0000\r\n\r\n"sv.size(), 'a'));
    CHECK(second == "real");

    // the first connection wasn't reused.
    CHECK(server.accepted == 2);
}

TEST_CASE("a connection whose server sent close_notify with a body's end isn't reused", "[tls][http][client]")
{
    net::io::scheduler sched{make_pool()};

    // the response and close_notify in one write: queued together, before either's flushed.
    const auto respond = [](tls::stream& s, int /*n*/) -> task<bool>
    {
        auto write = s.write(std::as_bytes(std::span{"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"sv}));
        (void)co_await s.shutdown();
        (void)co_await std::move(write);
        co_return false;
    };
    scripted_server   server{&sched, respond};
    net::http::client client{&sched, trusting_options()};

    // NOTE: POSTs, which aren't sent again if a reused connection turns out to have been closed.
    const auto test = [](net::http::client* client, std::string url) -> task<std::pair<std::string, std::string>>
    {
        const auto post = [](net::http::client* client, std::string url) -> task<std::string>
        {
            auto uri = net::url::parse(url);
            if (!uri.has_value()) co_return "error: bad URL";

            auto resp = co_await client->send({.method = net::http::request_method::POST, .uri = std::move(*uri)});
            if (!resp.has_value()) co_return "error: " + resp.error().message();

            auto body = co_await net::io::read_all(*resp->body);
            co_return body.has_value() ? *body : "error: " + body.error().message();
        };

        auto first = co_await post(client, url);
        co_return std::pair{first, co_await post(client, url)};
    };

    auto [first, second] = sched.run_to_completion(test(&client, "https://localhost:" + server.port() + "/"));
    CHECK(first == "hello");
    CHECK(second == "hello");
    CHECK(server.accepted == 2);
}

TEST_CASE("a request on an idle connection the server closes with close_notify is sent again", "[tls][http][client]")
{
    net::io::scheduler sched{make_pool()};

    // the first request is answered; the second, on the same connection, isn't: the server closes it instead (as one
    // does an idle connection, just as the client reuses it).
    const auto respond = [](tls::stream& s, int n) -> task<bool>
    {
        if (n == 1) co_return false;

        auto res = co_await net::io::write_all(s, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"sv);
        co_return !res.err;
    };
    scripted_server   server{&sched, respond};
    net::http::client client{&sched, trusting_options()};

    const auto test = [](net::http::client* client, std::string url) -> task<std::pair<std::string, std::string>>
    {
        auto first = co_await fetch(client, url);
        co_return std::pair{first, co_await fetch(client, url)};
    };

    auto [first, second] = sched.run_to_completion(test(&client, "https://localhost:" + server.port() + "/"));
    CHECK(first == "hello");
    CHECK(second == "hello");
    CHECK(server.accepted == 2);
    CHECK(server.requests == 3);
}

TEST_CASE("an https body without Content-Length ends with close_notify, and is cut short without it",
          "[tls][http][client]")
{
    net::io::scheduler sched{make_pool()};

    const bool notify = GENERATE(true, false);
    CAPTURE(notify);

    // the body runs until the connection ends: with close_notify, or (cut short) without it.
    const auto respond = [notify](tls::stream& s, int /*n*/) -> task<bool>
    {
        co_await net::io::write_all(s, "HTTP/1.1 200 OK\r\n\r\npartial"sv);
        if (!notify) s.socket().shutdown_write();
        co_return false;
    };
    scripted_server   server{&sched, respond};
    net::http::client client{&sched, trusting_options()};

    const auto test = [](net::http::client* client, std::string url) -> task<std::pair<std::string, std::error_code>>
    {
        auto resp = co_await client->get(url);
        if (!resp.has_value()) co_return std::pair{"error", resp.error()};

        std::string                  body;
        std::array<std::byte, 1'024> buf{};
        while (true)
        {
            auto res = co_await resp->body->read(buf);
            body.append(reinterpret_cast<const char*>(buf.data()), res.count);
            if (res.err) co_return std::pair{body, res.err};
        }
    };

    auto [body, err] = sched.run_to_completion(test(&client, "https://localhost:" + server.port() + "/"));
    CAPTURE(err.message());
    CHECK(body == "partial");
    if (notify) CHECK(err == net::io::errc::end_of_stream);
    else CHECK(err == tls::errc::stream_truncated);
}

TEST_CASE("an https body with Content-Length cut short fails to read, even with close_notify, and isn't reused",
          "[tls][http][client]")
{
    net::io::scheduler sched{make_pool()};

    const bool notify = GENERATE(true, false);
    CAPTURE(notify);

    // half of the body, then the connection ends: with close_notify, or without it.
    const auto respond = [notify](tls::stream& s, int /*n*/) -> task<bool>
    {
        co_await net::io::write_all(s, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello"sv);
        if (!notify) s.socket().shutdown_write();
        co_return false;
    };
    scripted_server   server{&sched, respond};
    net::http::client client{&sched, trusting_options()};

    const auto test = [](net::http::client* client, std::string url) -> task<std::pair<std::string, std::error_code>>
    {
        auto resp = co_await client->get(url);
        if (!resp.has_value()) co_return std::pair{"error", resp.error()};

        std::string                  body;
        std::array<std::byte, 1'024> buf{};
        while (true)
        {
            auto res = co_await resp->body->read(buf);
            body.append(reinterpret_cast<const char*>(buf.data()), res.count);
            if (res.err) co_return std::pair{body, res.err};
        }
    };

    auto [body, err] = sched.run_to_completion(test(&client, "https://localhost:" + server.port() + "/"));
    CAPTURE(err.message());
    CHECK(body == "hello");
    if (notify) CHECK(err == std::errc::connection_aborted);
    else CHECK(err == tls::errc::stream_truncated);
    CHECK(client.stats() == net::http::connection_stats{});
}
