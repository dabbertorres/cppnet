#include "http/client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <netinet/in.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "dns/blocking_lookup.hpp"
#include "http/connection_pool.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/server.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/string_reader.hpp"
#include "io/util.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "url.hpp"
#include "util/defer.hpp"

namespace
{

using namespace std::string_view_literals;

using net::coro::task;

std::string local_port(const net::listener& listener)
{
    sockaddr_in addr{};
    socklen_t   size = sizeof(addr);
    REQUIRE(::getsockname(listener.native_handle(), reinterpret_cast<sockaddr*>(&addr), &size) == 0);
    return std::to_string(ntohs(addr.sin_port));
}

// serve_once accepts one connection, records the request's head, and sends a canned response.
task<> serve_once(net::listener* listener, std::string* request_head)
{
    auto conn = co_await listener->accept();
    if (!conn.has_value()) co_return;

    std::array<std::byte, 1'024> buf{};
    while (!request_head->contains("\r\n\r\n"))
    {
        auto res = co_await conn->read(buf);
        request_head->append(reinterpret_cast<const char*>(buf.data()), res.count);
        if (res.err) co_return;
    }

    co_await conn->write(std::as_bytes(std::span{"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"sv}));
}

// read_all reads a body until it ends.
task<std::string> read_all(net::io::reader* body)
{
    std::string                out;
    std::array<std::byte, 256> buf{};

    while (true)
    {
        auto res = co_await body->read(buf);
        out.append(reinterpret_cast<const char*>(buf.data()), res.count);
        if (res.err) co_return res.err == net::io::errc::end_of_stream ? out : out + " [" + res.err.message() + "]";
    }
}

}

TEST_CASE("the client sends a request, and reads the response", "[http][client]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    std::string request_head;
    REQUIRE(sched.spawn(serve_once(&listener, &request_head)));

    auto get = [](net::io::scheduler* sched, std::string url) -> task<std::pair<net::http::status, std::string>>
    {
        auto parsed = net::url::parse(url);
        if (!parsed.has_value()) co_return std::pair{net::http::status::NONE, std::string{"bad url"}};

        net::http::client         client{sched};
        net::http::client_request req{
            .method  = net::http::request_method::GET,
            .version = {.major = 1, .minor = 1},
            .uri     = parsed.value(),
        };

        auto resp = co_await client.send(std::move(req));
        if (!resp.has_value()) co_return std::pair{net::http::status::NONE, resp.error().message()};

        // the body is read after send() has returned: the connection must still be there.
        auto body = co_await read_all(resp->body.get());
        co_return std::pair{resp->status_code, body};
    };

    auto port           = local_port(listener);
    auto [status, body] = sched.run_to_completion(get(&sched, "http://127.0.0.1:" + port + "/path?q=1"));

    CHECK(status == net::http::status::OK);
    CHECK(body == "hello");
    CHECK(request_head.starts_with("GET /path?q=1 HTTP/1.1\r\n"));
    CHECK(request_head.contains("\r\nHost: 127.0.0.1:" + port + "\r\n"));
}

TEST_CASE("get() sends a GET, over HTTP/1.1", "[http][client]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    std::string request_head;
    REQUIRE(sched.spawn(serve_once(&listener, &request_head)));

    auto get = [](net::io::scheduler* sched, std::string url) -> task<std::string>
    {
        net::http::client client{sched};

        auto resp = co_await client.get(url);
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        auto body = co_await net::io::read_all(*resp->body);
        co_return body.has_value() ? *body : "error: " + body.error().message();
    };

    auto port = local_port(listener);
    CHECK(sched.run_to_completion(get(&sched, "http://127.0.0.1:" + port + "/path?q=1")) == "hello");
    CHECK(request_head.starts_with("GET /path?q=1 HTTP/1.1\r\n"));
}

TEST_CASE("the client sends a URL's path and query as given", "[http][client]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    // as a signed URL's: encodings (of '/' too, in either case) and '+' kept, and the parameters in their order.
    constexpr auto target = "/files/a%2Fb%2fc/x+y?Signature=ab+c/d%3D&Expires=1&a=%2F%2f&Key-Pair-Id=K"sv;

    std::string   received;
    std::uint32_t version = 0;

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    net::http::server  server{&sched,
                              net::listener::make(&sched, "127.0.0.1", "0").value(),
                              [&](net::http::server_request& req, net::http::response_writer&) -> task<>
                              {
                                 received = req.target.path + "?" + req.target.query;
                                 version  = req.version.major;
                                 co_return;
                              }};
    REQUIRE(sched.spawn(server.serve()));

    auto get = [](net::io::scheduler* sched, std::string url, bool http2) -> task<std::string>
    {
        net::http::client_options options;
        options.http2_prior_knowledge = http2;
        net::http::client client{sched, options};

        auto resp = co_await client.get(url);
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        auto body = co_await net::io::read_all(*resp->body);
        co_return body.has_value() ? std::format("{}", static_cast<int>(resp->status_code))
                                   : "error: " + body.error().message();
    };

    const auto url = std::format("http://127.0.0.1:{}{}", server.local_port(), target);
    CHECK(sched.run_to_completion(get(&sched, url, http2)) == "200");
    CHECK(received == target);
    CHECK(version == (http2 ? 2U : 1U));
}

#ifndef NET_HAS_TLS
TEST_CASE("without TLS, https isn't supported", "[http][client]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    auto get = [](net::io::scheduler* sched) -> task<std::error_code>
    {
        net::http::client client{sched};

        auto resp = co_await client.get("https://127.0.0.1/");
        co_return resp.has_value() ? std::error_code{} : resp.error();
    };

    CHECK(sched.run_to_completion(get(&sched)) == std::errc::protocol_not_supported);
}
#endif

TEST_CASE("requests must be valid to be sent", "[http][client]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    auto send = [](net::io::scheduler* sched) -> task<std::pair<std::error_code, std::error_code>>
    {
        net::http::client client{sched};

        auto bad_url = co_await client.get("http://[::1");

        net::http::client_request no_method{.method = net::http::request_method::NONE};
        no_method.uri.host = "127.0.0.1";
        auto sent          = co_await client.send(std::move(no_method));

        co_return std::pair{bad_url.has_value() ? std::error_code{} : bad_url.error(),
                            sent.has_value() ? std::error_code{} : sent.error()};
    };

    auto [bad_url, no_method] = sched.run_to_completion(send(&sched));
    CHECK(bad_url == std::errc::invalid_argument);
    CHECK(no_method == std::errc::invalid_argument);
}

TEST_CASE("the client only sends HTTP/1.x", "[http][client]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    auto send = [](net::io::scheduler* sched) -> task<std::error_code>
    {
        net::http::client         client{sched};
        net::http::client_request req{
            .method  = net::http::request_method::GET,
            .version = {.major = 2, .minor = 0}
        };
        req.uri.host = "127.0.0.1";

        auto resp = co_await client.send(std::move(req));
        co_return resp.has_value() ? std::error_code{} : resp.error();
    };

    CHECK(sched.run_to_completion(send(&sched)) == std::errc::not_supported);
}

namespace
{

using namespace std::chrono_literals;

// keepalive_server serves "hello" to every request, on as many connections as it's given.
struct keepalive_server
{
    enum class closing : std::uint8_t
    {
        never,
        with_header,    // "Connection: close" on each response, then close
        without_header, // close after each response, as a server might when its own idle timeout passes
        on_second,      // close on a connection's second request, without responding
        short_body,     // close after half of each response's body
    };

    explicit keepalive_server(net::io::scheduler* sched)
        : sched{sched}
        , listener{sched, "127.0.0.1", "0", net::network::tcp}
    {
        listener.listen(64);
        port = local_port(listener);
    }

    task<> serve()
    {
        while (true)
        {
            auto conn = co_await listener.accept();
            if (!conn.has_value()) co_return;

            if (!sched->spawn(handle(std::move(*conn)))) co_return;
        }
    }

    task<> handle(net::tcp_socket conn)
    {
        ++accepted;
        auto now_active = ++active;
        for (auto seen = max_active.load(); now_active > seen && !max_active.compare_exchange_weak(seen, now_active);)
        {}

        std::string                  pending;
        std::array<std::byte, 1'024> buf{};
        int                          on_connection = 0;

        while (true)
        {
            // one request head (they have no bodies).
            while (!pending.contains("\r\n\r\n"))
            {
                auto res = co_await conn.read(buf);
                pending.append(reinterpret_cast<const char*>(buf.data()), res.count);
                if (res.err) goto done;
            }
            pending.erase(0, pending.find("\r\n\r\n") + 4);
            ++requests;

            if (when == closing::on_second && ++on_connection == 2)
            {
                // NOTE: before closing, so the client sees its request canceled whenever it sees the close.
                if (stop_on_second != nullptr) stop_on_second->request_stop();
                break;
            }

            if (delay > 0ms) (void)co_await sched->sleep_until(net::io::clock::now() + delay);

            auto response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"sv;
            if (when == closing::with_header)
                response = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello"sv;
            else if (when == closing::short_body) response = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello"sv;
            co_await conn.write(std::as_bytes(std::span{response}));

            if (when == closing::with_header || when == closing::without_header || when == closing::short_body) break;
        }

done:
        --active;
    }

    net::io::scheduler* sched;
    net::listener       listener;
    std::string         port;

    closing                   when           = closing::never;
    std::chrono::milliseconds delay          = 0ms;
    std::stop_source*         stop_on_second = nullptr; // stopped by closing::on_second as it closes

    std::atomic<int> accepted{0};
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
    std::atomic<int> requests{0};
};

net::http::client_request get_request(const std::string& host, const std::string& port)
{
    net::http::client_request req{
        .method  = net::http::request_method::GET,
        .version = {.major = 1, .minor = 1}
    };
    req.uri.scheme = "http";
    req.uri.host   = host;
    req.uri.port   = port;
    req.uri.path   = "/";
    return req;
}

// fetch sends a GET, and reads its body to the end. It returns the body, or the error.
task<std::string> fetch(net::http::client* client, std::string host, std::string port)
{
    auto resp = co_await client->send(get_request(host, port));
    if (!resp.has_value()) co_return "error: " + resp.error().message();

    co_return co_await read_all(resp->body.get());
}

// fetch_all fetches from each host at once, and returns the results in order.
task<std::vector<std::string>>
fetch_all(net::io::scheduler* sched, net::http::client* client, std::vector<std::string> hosts, std::string port)
{
    std::vector<std::string>   results(hosts.size());
    std::atomic<std::uint64_t> finished{0};

    auto one = [](net::io::scheduler*         sched,
                  net::http::client*          client,
                  std::string                 host,
                  std::string                 port,
                  std::string*                result,
                  std::atomic<std::uint64_t>* finished) -> task<>
    {
        *result = co_await fetch(client, host, port);
        ++*finished;
        sched->notify_all(*finished);
    };

    for (std::size_t i = 0; i < hosts.size(); ++i)
        if (!sched->spawn(one(sched, client, hosts[i], port, &results[i], &finished)))
            co_return std::vector<std::string>{"spawn failed"};

    for (auto seen = finished.load(); seen < hosts.size(); seen = finished.load())
        if (auto err = co_await sched->wait_on(finished, seen); err) co_return std::vector{"wait: " + err.message()};

    co_return results;
}

// run serves on server, and runs test, until it finishes.
template<typename T>
T run(net::io::scheduler& sched, keepalive_server& server, task<T> test)
{
    REQUIRE(sched.spawn(server.serve()));
    return sched.run_to_completion(std::move(test));
}

}

TEST_CASE("a request made for send() lasts until its task is awaited", "[http][client]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};
    net::http::client  client{&sched};

    // the request is a temporary, gone before the task starts (which ASan would catch, if send() referred to it).
    auto test = [](net::http::client* client, std::string port) -> task<std::string>
    {
        auto sending = client->send(get_request("127.0.0.1", port));
        auto resp    = co_await std::move(sending);
        if (!resp.has_value()) co_return "error: " + resp.error().message();
        co_return co_await read_all(resp->body.get());
    };

    CHECK(run(sched, server, test(&client, server.port)) == "hello");
}

TEST_CASE("connections are reused", "[http][client][pool]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};
    net::http::client  client{&sched};

    auto test = [](net::http::client* client, std::string port) -> task<std::vector<std::string>>
    {
        std::vector<std::string> bodies;
        bodies.reserve(3);
        for (int i = 0; i < 3; ++i) bodies.push_back(co_await fetch(client, "127.0.0.1", port));
        co_return bodies;
    };

    CHECK(run(sched, server, test(&client, server.port)) == std::vector<std::string>(3, "hello"));
    CHECK(server.accepted == 1);
    CHECK(server.requests == 3);
    CHECK(client.stats() == net::http::connection_stats{.open = 1, .idle = 1});
}

TEST_CASE("connections aren't reused if they can't be", "[http][client][pool]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};

    SECTION("the body wasn't read to the end")
    {
        net::http::client client{&sched};

        auto test = [](net::http::client* client, std::string port) -> task<std::string>
        {
            for (int i = 0; i < 2; ++i)
            {
                auto resp = co_await client->send(get_request("127.0.0.1", port));
                if (!resp.has_value()) co_return resp.error().message();
            }
            co_return "ok";
        };

        CHECK(run(sched, server, test(&client, server.port)) == "ok");
        CHECK(server.accepted == 2);
        CHECK(client.stats() == net::http::connection_stats{});
    }

    SECTION("Connection: close")
    {
        server.when = keepalive_server::closing::with_header;
        net::http::client client{&sched};

        auto test = [](net::http::client* client, std::string port) -> task<std::string>
        { co_return co_await fetch(client, "127.0.0.1", port) + co_await fetch(client, "127.0.0.1", port); };

        CHECK(run(sched, server, test(&client, server.port)) == "hellohello");
        CHECK(server.accepted == 2);
    }

    SECTION("keep_alive is off")
    {
        net::http::client client{&sched, {.keep_alive = false}};

        auto test = [](net::http::client* client, std::string port) -> task<std::string>
        { co_return co_await fetch(client, "127.0.0.1", port) + co_await fetch(client, "127.0.0.1", port); };

        CHECK(run(sched, server, test(&client, server.port)) == "hellohello");
        CHECK(server.accepted == 2);
        CHECK(client.stats() == net::http::connection_stats{});
    }
}

TEST_CASE("a response body cut short by the server fails to read, and its connection isn't reused",
          "[http][client][pool]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};
    server.when = keepalive_server::closing::short_body;
    net::http::client client{&sched};

    auto test = [](net::http::client* client, std::string port) -> task<std::string>
    { co_return co_await fetch(client, "127.0.0.1", port); };

    // read_all marks a body that failed, rather than ended, with its error.
    const auto expected = "hello [" + std::make_error_code(std::errc::connection_aborted).message() + "]";
    CHECK(run(sched, server, test(&client, server.port)) == expected);
    CHECK(client.stats() == net::http::connection_stats{});
}

TEST_CASE("idle connections the server closed aren't reused", "[http][client][pool]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};
    server.when = keepalive_server::closing::without_header;

    net::http::client client{&sched};

    auto test = [](net::io::scheduler* sched, net::http::client* client, std::string port) -> task<std::string>
    {
        std::string bodies;
        for (int i = 0; i < 5; ++i)
        {
            bodies += co_await fetch(client, "127.0.0.1", port);

            // every other time, give the server's close time to arrive, so it's seen before reusing the connection.
            // Otherwise, the request fails on the closed connection, and is retried.
            if (i % 2 == 0) (void)co_await sched->sleep_until(net::io::clock::now() + 20ms);
        }
        co_return bodies;
    };

    CHECK(run(sched, server, test(&sched, &client, server.port)) == "hellohellohellohellohello");
    CHECK(server.accepted == 5);
    CHECK(server.requests == 5);
}

TEST_CASE("connections per host are limited", "[http][client][pool]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};
    server.delay = 50ms;

    net::http::client client{&sched, {.max_connections_per_host = 2}};

    auto results =
        run(sched, server, fetch_all(&sched, &client, std::vector<std::string>(6, "127.0.0.1"), server.port));

    CHECK(results == std::vector<std::string>(6, "hello"));
    CHECK(server.accepted == 2);
    CHECK(server.max_active == 2);
    CHECK(server.requests == 6);
}

TEST_CASE("connections altogether are limited", "[http][client][pool]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};
    server.delay = 50ms;

    net::http::client client{&sched, {.max_connections = 1}};

    SECTION("waiting for a connection")
    {
        // two names for the same server, so two hosts as far as the client knows.
        auto results =
            run(sched,
                server,
                fetch_all(&sched, &client, {"127.0.0.1", "localhost", "127.0.0.1", "localhost"}, server.port));

        CHECK(results == std::vector<std::string>(4, "hello"));
        CHECK(server.max_active == 1);
    }

    SECTION("another host's idle connection is closed to make room")
    {
        auto test = [](net::http::client* client, std::string port) -> task<std::string>
        { co_return co_await fetch(client, "127.0.0.1", port) + co_await fetch(client, "localhost", port); };

        CHECK(run(sched, server, test(&client, server.port)) == "hellohello");
        CHECK(client.stats("127.0.0.1", server.port) == net::http::connection_stats{});
        CHECK(client.stats("localhost", server.port) == net::http::connection_stats{.open = 1, .idle = 1});
    }
}

TEST_CASE("waiting for a connection times out", "[http][client][pool]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};
    server.delay = 500ms;

    net::http::client client{
        &sched,
        {.connect_timeout = 100ms, .max_connections_per_host = 1}
    };

    auto results = run(sched, server, fetch_all(&sched, &client, {"127.0.0.1", "127.0.0.1"}, server.port));

    std::ranges::sort(results);
    CHECK(results
          == std::vector<std::string>{"error: " + std::make_error_code(std::errc::timed_out).message(), "hello"});
}

TEST_CASE("idle connections are closed after idle_timeout", "[http][client][pool]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};

    net::http::client client{&sched, {.idle_timeout = 100ms}};

    auto test = [](net::io::scheduler* sched,
                   net::http::client*  client,
                   keepalive_server*   server) -> task<std::pair<net::http::connection_stats, int>>
    {
        co_await fetch(client, "127.0.0.1", server->port);
        (void)co_await sched->sleep_until(net::io::clock::now() + 300ms);

        // the server has seen it close.
        co_return std::pair{client->stats(), server->active.load()};
    };

    auto [stats, active] = run(sched, server, test(&sched, &client, &server));
    CHECK(stats == net::http::connection_stats{});
    CHECK(active == 0);
}

TEST_CASE("requests are retried if a reused connection was closed", "[http][client][pool]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};
    server.when = keepalive_server::closing::on_second;

    net::http::client client{&sched};

    SECTION("GET is retried")
    {
        auto test = [](net::http::client* client, std::string port) -> task<std::string>
        { co_return co_await fetch(client, "127.0.0.1", port) + co_await fetch(client, "127.0.0.1", port); };

        CHECK(run(sched, server, test(&client, server.port)) == "hellohello");
        CHECK(server.accepted == 2);
        CHECK(server.requests == 3);
    }

    SECTION("POST isn't, as it may not be safe to repeat")
    {
        auto test = [](net::http::client* client, std::string port) -> task<std::string>
        {
            auto first = co_await fetch(client, "127.0.0.1", port);

            auto post   = get_request("127.0.0.1", port);
            post.method = net::http::request_method::POST;
            post.headers.set_content_length(0);

            auto resp = co_await client->send(std::move(post));
            co_return first + (resp.has_value() ? "sent" : " failed");
        };

        CHECK(run(sched, server, test(&client, server.port)) == "hello failed");
        CHECK(server.accepted == 1);
    }
}

namespace
{

// echo_server responds to each request with its body, or (if rejecting) with 413 before reading the body.
struct echo_server
{
    explicit echo_server(net::io::scheduler* sched)
        : sched{sched}
        , listener{sched, "127.0.0.1", "0", net::network::tcp}
    {
        listener.listen(64);
        port = local_port(listener);
    }

    task<> serve()
    {
        while (true)
        {
            auto conn = co_await listener.accept();
            if (!conn.has_value()) co_return;

            if (!sched->spawn(handle(std::move(*conn)))) co_return;
        }
    }

    task<> handle(net::tcp_socket conn)
    {
        ++accepted;

        net::io::buffered_reader reader{&conn};

        while (true)
        {
            auto req = co_await net::http::http11::request_decode(&reader, 8'192);
            if (!req.has_value()) break;

            ++requests;
            if (req->headers.is_chunked()) ++chunked;

            if (reject)
            {
                auto response = "HTTP/1.1 413 Content Too Large\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"sv;
                co_await conn.write(std::as_bytes(std::span{response}));

                if (close_at_once) break;

                // then read (and drop) the rest, as closing with it unread would reset the connection.
                conn.shutdown_write();
                std::array<std::byte, 16'384> buf{};
                while (!(co_await conn.read(buf)).err)
                {}
                break;
            }

            auto body     = co_await read_all(req->body.get());
            auto response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
            co_await conn.write(std::as_bytes(std::span{response}));
        }
    }

    net::io::scheduler* sched;
    net::listener       listener;
    std::string         port;
    bool                reject        = false;
    bool                close_at_once = false; // after rejecting

    std::atomic<int> accepted{0};
    std::atomic<int> requests{0};
    std::atomic<int> chunked{0};
};

net::http::client_request post_request(const std::string& port)
{
    auto req   = get_request("127.0.0.1", port);
    req.method = net::http::request_method::POST;
    return req;
}

// post sends req, and returns the response's status and body, or the error.
task<std::string> post(net::http::client* client, net::http::client_request req)
{
    auto resp = co_await client->send(std::move(req));
    if (!resp.has_value()) co_return "error: " + resp.error().message();

    auto body = co_await read_all(resp->body.get());
    co_return std::to_string(static_cast<int>(resp->status_code)) + " " + body;
}

}

TEST_CASE("request bodies are sent", "[http][client][body]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    echo_server        server{&sched};
    net::http::client  client{&sched};

    std::string big;
    for (int i = 0; big.size() < 200'000; ++i) big += std::to_string(i) + ",";

    auto test = [](net::http::client* client, std::string port, std::string big) -> task<std::vector<std::string>>
    {
        std::vector<std::string> results;

        auto with_length = post_request(port);
        with_length.set_body("hello");
        results.push_back(co_await post(client, std::move(with_length)));

        auto chunked = post_request(port);
        chunked.body = std::make_unique<net::io::owned_string_reader>(big);
        results.push_back(co_await post(client, std::move(chunked)));

        results.push_back(co_await post(client, post_request(port)));

        co_return results;
    };

    REQUIRE(sched.spawn(server.serve()));
    auto results = sched.run_to_completion(test(&client, server.port, big));

    CHECK(results == std::vector<std::string>{"200 hello", "200 " + big, "200 "});
    CHECK(server.chunked == 1);

    // each body was framed right, so the connection carried them all.
    CHECK(server.accepted == 1);
}

TEST_CASE("a response before the whole body was sent is still read", "[http][client][body]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    echo_server        server{&sched};
    server.reject = true;

    // if the server reads the rest, the body is all written. If it doesn't, writing it fails (once the server has
    // closed), but the response is there to read.
    server.close_at_once = GENERATE(false, true);
    CAPTURE(server.close_at_once);

    net::http::client client{&sched};

    auto req = post_request(server.port);
    req.set_body(std::string(std::size_t{4} * 1'024 * 1'024, 'x'));

    REQUIRE(sched.spawn(server.serve()));
    // NOTE: not moved inside CHECK, which expands its expression twice (once unevaluated).
    auto result = sched.run_to_completion(post(&client, std::move(req)));
    CHECK(result == "413 ");
    CHECK(client.stats() == net::http::connection_stats{});
}

TEST_CASE("requests with bodies that can't be sent don't use a connection", "[http][client][body]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    echo_server        server{&sched};
    net::http::client  client{&sched};

    auto req = post_request(server.port);
    req.headers.set_content_length(5);

    REQUIRE(sched.spawn(server.serve()));
    auto result = sched.run_to_completion(post(&client, std::move(req)));
    CHECK(result == "error: " + std::make_error_code(std::errc::invalid_argument).message());
    CHECK(server.accepted == 0);
}

namespace
{

// stalling_server stalls part way through each request.
struct stalling_server
{
    enum class stall : std::uint8_t
    {
        before_responding, // reads the request's head, but never responds
        in_the_body,       // sends half of the body
        before_reading,    // never reads the request
    };

    stalling_server(net::io::scheduler* sched, stall where)
        : sched{sched}
        , listener{sched, "127.0.0.1", "0", net::network::tcp}
        , where{where}
    {
        listener.listen(64);
        port = local_port(listener);
    }

    task<> serve() const
    {
        while (true)
        {
            auto conn = co_await listener.accept();
            if (!conn.has_value()) co_return;

            if (!sched->spawn(handle(std::move(*conn)))) co_return;
        }
    }

    task<> handle(net::tcp_socket conn) const
    {
        std::array<std::byte, 16'384> buf{};

        if (where == stall::before_reading)
        {
            // until the test ends (which cancels it).
            (void)co_await sched->sleep_until(net::io::clock::now() + 1h);
            co_return;
        }

        std::string head;
        while (!head.contains("\r\n\r\n"))
        {
            auto res = co_await conn.read(buf);
            head.append(reinterpret_cast<const char*>(buf.data()), res.count);
            if (res.err) co_return;
        }

        if (where == stall::in_the_body)
        {
            auto partial = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello"sv;
            co_await conn.write(std::as_bytes(std::span{partial}));
        }

        // then nothing, until the client gives up (and closes the connection).
        while (!(co_await conn.read(buf)).err)
        {}
    }

    net::io::scheduler* sched;
    net::listener       listener;
    std::string         port;
    stall               where;
};

// endless_reader is a body that never ends.
class endless_reader : public net::io::reader
{
public:
    net::coro::task<net::io::result> read(std::span<std::byte> data) override
    {
        std::ranges::fill(data, std::byte{'x'});
        co_return {.count = data.size()};
    }

    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }
};

// timed runs test, and returns its result, and how long it took.
template<typename T>
std::pair<T, net::io::clock::duration> timed(net::io::scheduler& sched, task<T> test)
{
    auto start  = net::io::clock::now();
    auto result = sched.run_to_completion(std::move(test));
    return {std::move(result), net::io::clock::now() - start};
}

std::string timed_out() { return "error: " + std::make_error_code(std::errc::timed_out).message(); }
}

TEST_CASE("waiting for a response's head times out", "[http][client][timeout]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    stalling_server    server{&sched, stalling_server::stall::before_responding};
    net::http::client  client{&sched, {.response_header_timeout = 100ms}};

    REQUIRE(sched.spawn(server.serve()));
    auto [result, elapsed] = timed(sched, fetch(&client, "127.0.0.1", server.port));

    CHECK(result == timed_out());
    CHECK(elapsed >= 100ms);
    CHECK(elapsed < 5s);
    CHECK(client.stats() == net::http::connection_stats{});
}

TEST_CASE("a stalled body times out", "[http][client][timeout]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    stalling_server    server{&sched, stalling_server::stall::in_the_body};
    net::http::client  client{&sched, {.io_timeout = 100ms}};

    REQUIRE(sched.spawn(server.serve()));
    auto [result, elapsed] = timed(sched, fetch(&client, "127.0.0.1", server.port));

    // what arrived, and then why the rest didn't.
    CHECK(result == "hello [" + std::make_error_code(std::errc::timed_out).message() + "]");
    CHECK(elapsed >= 100ms);
    CHECK(elapsed < 5s);
    CHECK(client.stats() == net::http::connection_stats{});
}

TEST_CASE("a stalled request body times out", "[http][client][timeout]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    stalling_server    server{&sched, stalling_server::stall::before_reading};
    net::http::client  client{&sched, {.io_timeout = 100ms}};

    auto req = post_request(server.port);
    req.body = std::make_unique<endless_reader>();

    REQUIRE(sched.spawn(server.serve()));
    auto [result, elapsed] = timed(sched, post(&client, std::move(req)));

    // NOTE: it's only a stall once the connection's buffers are full, which can take a while: e.g. Linux grows its
    //       receive window a little at a time, so writes keep making some progress (~2.6MB over ~5s here, with debug
    //       builds, where macOS takes ~0.5MB, and ~0.1s).
    CHECK(result == timed_out());
    CHECK(elapsed >= 100ms);
    CHECK(elapsed < 30s);
}

TEST_CASE("timeouts don't limit what makes progress", "[http][client][timeout]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    echo_server        server{&sched};
    net::http::client  client{
        &sched,
        {.response_header_timeout = 500ms, .io_timeout = 500ms}
    };

    // big enough to take more than one wait to send and receive.
    std::string big(std::size_t{8} * 1'024 * 1'024, 'x');

    auto req = post_request(server.port);
    req.set_body(big);

    REQUIRE(sched.spawn(server.serve()));
    auto result = sched.run_to_completion(post(&client, std::move(req)));
    CHECK(result == "200 " + big);
}

namespace
{

std::string canceled() { return "error: " + std::make_error_code(std::errc::operation_canceled).message(); }

// stop_after requests stop after delay (as one of the scheduler's tasks, holding none of its locks).
task<> stop_after(net::io::scheduler* sched, std::stop_source source, net::io::clock::duration delay)
{
    (void)co_await sched->sleep_until(net::io::clock::now() + delay);
    source.request_stop();
}

// fetch_until sends a GET, canceled by stop, and reads its body to the end. It returns the body (and the error that
// ended it, if any), or the error.
task<std::string> fetch_until(net::http::client* client, std::string host, std::string port, std::stop_token stop)
{
    auto req = get_request(host, port);
    req.stop = std::move(stop);

    auto resp = co_await client->send(std::move(req));
    if (!resp.has_value()) co_return "error: " + resp.error().message();

    co_return co_await read_all(resp->body.get());
}

}

TEST_CASE("a request stopped before it's sent fails without a connection", "[http][client][cancel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    keepalive_server   server{&sched};
    net::http::client  client{&sched};

    std::stop_source source;
    source.request_stop();

    CHECK(run(sched, server, fetch_until(&client, "127.0.0.1", server.port, source.get_token())) == canceled());
    CHECK(server.accepted == 0);
    CHECK(client.stats() == net::http::connection_stats{});
}

TEST_CASE("a request canceled while it waits for a connection gives up its place", "[http][client][cancel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};
    server.delay = 300ms;

    net::http::client client{
        &sched,
        {.connect_timeout = 10s, .max_connections_per_host = 1}
    };

    const auto test = [](net::io::scheduler* sched,
                         net::http::client*  client,
                         keepalive_server*   server) -> task<std::vector<std::string>>
    {
        // the one connection is busy (its response delayed), so the next request waits for it, until it's canceled.
        std::string                busy;
        std::atomic<std::uint64_t> done{0};
        REQUIRE(sched->spawn(
            [](net::http::client*          client,
               std::string                 port,
               std::string*                out,
               std::atomic<std::uint64_t>* done,
               net::io::scheduler*         sched) -> task<>
            {
                *out = co_await fetch(client, "127.0.0.1", port);
                done->store(1);
                sched->notify_all(*done);
            }(client, server->port, &busy, &done, sched)));

        while (server->requests.load() == 0) (void)co_await sched->sleep_until(net::io::clock::now() + 1ms);

        std::stop_source source;
        REQUIRE(sched->spawn(stop_after(sched, source, 50ms)));
        std::vector<std::string> out{co_await fetch_until(client, "127.0.0.1", server->port, source.get_token())};

        // canceled before the busy one finished.
        out.emplace_back(done.load() == 0 ? "first still busy" : "first done");
        while (done.load() == 0) (void)co_await sched->wait_on(done, 0);
        out.push_back(busy);

        // and the connection's free for the next.
        out.push_back(co_await fetch(client, "127.0.0.1", server->port));
        co_return out;
    };

    const auto results = run(sched, server, test(&sched, &client, &server));
    CHECK(results == std::vector<std::string>{canceled(), "first still busy", "hello", "hello"});
    CHECK(server.accepted == 1);
    CHECK(client.stats() == net::http::connection_stats{.open = 1, .idle = 1});
}

TEST_CASE("a request canceled while it connects counts its connection out", "[http][client][cancel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    net::http::client  client{&sched};

    std::stop_source source;
    REQUIRE(sched.spawn(stop_after(&sched, source, 100ms)));

    // a non-routable address: depending on the network, connecting either hangs (until stop is requested), or fails
    // straight away.
    const auto start   = net::io::clock::now();
    const auto result  = sched.run_to_completion(fetch_until(&client, "10.255.255.1", "81", source.get_token()));
    const auto elapsed = net::io::clock::now() - start;

    CAPTURE(result);
    CHECK(result.starts_with("error: "));
    CHECK(elapsed < 5s);
    if (result == canceled()) CHECK(elapsed >= 100ms);
    CHECK(client.stats() == net::http::connection_stats{});
}

TEST_CASE("a request canceled while its host is resolved fails at once", "[http][client][cancel]")
{
    test::blocking_lookup lookup;

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    net::http::client client{&sched, {.resolver = &lookup.resolver}};

    const auto test = [](net::io::scheduler*    sched,
                         net::http::client*     client,
                         test::blocking_lookup* lookup) -> task<std::pair<std::string, net::io::clock::duration>>
    {
        std::stop_source source;

        // stops once the lookup has started, a call for each family (or the scheduler has stopped).
        REQUIRE(sched->spawn(
            [](net::io::scheduler* sched, test::blocking_lookup* lookup, std::stop_source source) -> task<>
            {
                while (lookup->started.load() < 2)
                {
                    if (co_await sched->sleep_until(net::io::clock::now() + 1ms)) co_return;
                }
                source.request_stop();
            }(sched, lookup, source)));

        const auto start  = net::io::clock::now();
        auto       result = co_await fetch_until(client, "slow.test", "80", source.get_token());
        const auto took   = net::io::clock::now() - start;

        lookup->release();
        co_return std::pair{std::move(result), took};
    };

    const auto [result, elapsed] = sched.run_to_completion(test(&sched, &client, &lookup));

    CHECK(result == canceled());
    CHECK(elapsed < 5s);
    CHECK(lookup.started.load() == 2);
    CHECK(client.stats() == net::http::connection_stats{});
}

TEST_CASE("connect_timeout bounds resolving the host", "[http][client][timeout]")
{
    test::blocking_lookup lookup;

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    net::http::client client{
        &sched,
        {.connect_timeout = 200ms, .resolver = &lookup.resolver}
    };

    const auto test = [](net::http::client*     client,
                         test::blocking_lookup* lookup) -> task<std::pair<std::string, net::io::clock::duration>>
    {
        const auto start  = net::io::clock::now();
        auto       result = co_await fetch(client, "slow.test", "80");
        const auto took   = net::io::clock::now() - start;

        lookup->release();
        co_return std::pair{std::move(result), took};
    };

    const auto [result, elapsed] = sched.run_to_completion(test(&client, &lookup));

    CHECK(result == "error: " + std::make_error_code(std::errc::timed_out).message());
    CHECK(elapsed >= 200ms);
    CHECK(elapsed < 5s);
    CHECK(lookup.started.load() == 2); // a call for each family
    CHECK(client.stats() == net::http::connection_stats{});
}

TEST_CASE("an HTTP/1.1 request canceled closes its connection", "[http][client][cancel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    SECTION("waiting for the response's head")
    {
        stalling_server   server{&sched, stalling_server::stall::before_responding};
        net::http::client client{&sched};

        std::stop_source source;
        REQUIRE(sched.spawn(server.serve()));
        REQUIRE(sched.spawn(stop_after(&sched, source, 100ms)));
        auto [result, elapsed] = timed(sched, fetch_until(&client, "127.0.0.1", server.port, source.get_token()));

        CHECK(result == canceled());
        CHECK(elapsed >= 100ms);
        CHECK(elapsed < 5s);
        CHECK(client.stats() == net::http::connection_stats{});
    }

    SECTION("reading the response's body")
    {
        stalling_server   server{&sched, stalling_server::stall::in_the_body};
        net::http::client client{&sched};

        const auto test =
            [](net::io::scheduler* sched, net::http::client* client, std::string port) -> task<std::string>
        {
            std::stop_source source;
            auto             req = get_request("127.0.0.1", port);
            req.stop             = source.get_token();

            auto resp = co_await client->send(std::move(req));
            if (!resp.has_value()) co_return "error: " + resp.error().message();

            // the token still cancels the body's reads, after send() returned: what arrived, then why the rest
            // didn't, and again for the read after.
            REQUIRE(sched->spawn(stop_after(sched, source, 100ms)));
            auto out = co_await read_all(resp->body.get());

            std::array<std::byte, 16> buf{};
            const auto                again = co_await resp->body->read(buf);
            co_return out + " " + again.err.message();
        };

        REQUIRE(sched.spawn(server.serve()));
        auto [result, elapsed] = timed(sched, test(&sched, &client, server.port));

        const auto message = std::make_error_code(std::errc::operation_canceled).message();
        CHECK(result == std::format("hello [{}] {}", message, message));
        CHECK(elapsed >= 100ms);
        CHECK(elapsed < 5s);
        CHECK(client.stats() == net::http::connection_stats{});
    }

    SECTION("with its body arrived, but not read")
    {
        keepalive_server  server{&sched};
        net::http::client client{&sched};

        const auto test =
            [](net::io::scheduler* sched, net::http::client* client, std::string port) -> task<std::string>
        {
            std::stop_source source;
            auto             req = get_request("127.0.0.1", port);
            req.stop             = source.get_token();

            auto resp = co_await client->send(std::move(req));
            if (!resp.has_value()) co_return "error: " + resp.error().message();

            // (time for all of it to arrive.)
            (void)co_await sched->sleep_until(net::io::clock::now() + 50ms);
            source.request_stop();
            co_return co_await read_all(resp->body.get());
        };

        const auto result = run(sched, server, test(&sched, &client, server.port));
        CHECK(result == " [" + std::make_error_code(std::errc::operation_canceled).message() + "]");
        CHECK(client.stats() == net::http::connection_stats{});
    }

    SECTION("sending the request's body")
    {
        stalling_server   server{&sched, stalling_server::stall::before_reading};
        net::http::client client{&sched};

        std::stop_source source;
        auto             req = post_request(server.port);
        req.body             = std::make_unique<endless_reader>();
        req.stop             = source.get_token();

        REQUIRE(sched.spawn(server.serve()));
        REQUIRE(sched.spawn(stop_after(&sched, source, 100ms)));
        auto [result, elapsed] = timed(sched, post(&client, std::move(req)));

        CHECK(result == canceled());
        CHECK(elapsed >= 100ms);
        CHECK(elapsed < 30s);
        CHECK(client.stats() == net::http::connection_stats{});
    }
}

TEST_CASE("a response read to its end before its request's canceled keeps its connection", "[http][client][cancel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    keepalive_server   server{&sched};
    net::http::client  client{&sched};

    const auto test = [](net::http::client* client, std::string port) -> task<std::vector<std::string>>
    {
        std::stop_source source;
        auto             req = get_request("127.0.0.1", port);
        req.stop             = source.get_token();

        auto resp = co_await client->send(std::move(req));
        if (!resp.has_value()) co_return std::vector{"error: " + resp.error().message()};

        std::vector<std::string> out{co_await read_all(resp->body.get())};

        // its end was read: stopping now changes nothing.
        source.request_stop();
        std::array<std::byte, 16> buf{};
        const auto                again = co_await resp->body->read(buf);
        out.push_back(again.err.message());

        out.push_back(co_await fetch(client, "127.0.0.1", port));
        co_return out;
    };

    const auto results = run(sched, server, test(&client, server.port));
    CHECK(results
          == std::vector<std::string>{"hello", make_error_code(net::io::errc::end_of_stream).message(), "hello"});
    CHECK(server.accepted == 1);
    CHECK(client.stats() == net::http::connection_stats{.open = 1, .idle = 1});
}

TEST_CASE("a canceled request isn't sent again", "[http][client][cancel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    keepalive_server   server{&sched};
    server.when = keepalive_server::closing::on_second;

    // the second request's connection is closed without a response (which would be sent again, on another), as its
    // token's stopped.
    std::stop_source source;
    server.stop_on_second = &source;

    net::http::client client{&sched};

    const auto test =
        [](net::http::client* client, std::string port, std::stop_token stop) -> task<std::vector<std::string>>
    {
        std::vector<std::string> out{co_await fetch(client, "127.0.0.1", port)};
        out.push_back(co_await fetch_until(client, "127.0.0.1", port, std::move(stop)));
        co_return out;
    };

    const auto results = run(sched, server, test(&client, server.port, source.get_token()));
    CHECK(results == std::vector<std::string>{"hello", canceled()});
    CHECK(server.accepted == 1);
    CHECK(server.requests == 2);
    CHECK(client.stats() == net::http::connection_stats{});
}

TEST_CASE("a request racing its cancellation ends once, and keeps its connection only if it's complete",
          "[http][client][cancel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};
    net::http::client  client{&sched};

    const auto test =
        [](net::io::scheduler* sched, net::http::client* client, std::string port) -> task<std::vector<std::string>>
    {
        std::vector<std::string> unexpected;
        const auto               aborted = " [" + std::make_error_code(std::errc::operation_canceled).message() + "]";

        for (int round = 0; round < 300; ++round)
        {
            // stop before, during, or after the request (the delays straddle a round trip).
            std::stop_source source;
            const auto       delay = std::chrono::microseconds{(round % 10) * 30};
            if (round % 10 == 0) source.request_stop();
            else if (!sched->spawn(stop_after(sched, source, delay))) co_return std::vector<std::string>{"stopped"};

            auto result = co_await fetch_until(client, "127.0.0.1", port, source.get_token());
            if (result != "hello" && result != canceled() && result != aborted) unexpected.push_back(result);

            // whatever happened, the pool holds the connection only if it's idle, and only one.
            const auto stats = client->stats();
            if (stats.open != stats.idle || stats.open > 1)
                unexpected.push_back(std::format("{} open, {} idle", stats.open, stats.idle));
        }
        co_return unexpected;
    };

    const auto unexpected = run(sched, server, test(&sched, &client, server.port));
    CHECK(unexpected.empty());
}
