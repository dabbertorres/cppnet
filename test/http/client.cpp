#include "http/client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "url.hpp"

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
        if (res.err) co_return res.err == net::io::status_condition::closed ? out : out + " [" + res.err.message() + "]";
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
            .uri     = parsed.to_value(),
        };

        auto resp = co_await client.send(req);
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

TEST_CASE("the client only sends HTTP/1.x", "[http][client]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    auto send = [](net::io::scheduler* sched) -> task<std::error_condition>
    {
        net::http::client         client{sched};
        net::http::client_request req{.method = net::http::request_method::GET, .version = {.major = 2, .minor = 0}};
        req.uri.host = "127.0.0.1";

        auto resp = co_await client.send(req);
        co_return resp.has_value() ? std::error_condition{} : resp.error();
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

            if (when == closing::on_second && ++on_connection == 2) break;

            if (delay > 0ms) co_await sched->sleep_until(net::io::clock::now() + delay);

            auto response = when == closing::with_header
                              ? "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello"sv
                              : "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"sv;
            co_await conn.write(std::as_bytes(std::span{response}));

            if (when == closing::with_header || when == closing::without_header) break;
        }

    done:
        --active;
    }

    net::io::scheduler* sched;
    net::listener       listener;
    std::string         port;

    closing                   when  = closing::never;
    std::chrono::milliseconds delay = 0ms;

    std::atomic<int> accepted{0};
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
    std::atomic<int> requests{0};
};

net::http::client_request get_request(const std::string& host, const std::string& port)
{
    net::http::client_request req{.method = net::http::request_method::GET, .version = {.major = 1, .minor = 1}};
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

TEST_CASE("connections are reused", "[http][client][pool]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};
    net::http::client  client{&sched};

    auto test = [](net::http::client* client, std::string port) -> task<std::vector<std::string>>
    {
        std::vector<std::string> bodies;
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
            if (i % 2 == 0) co_await sched->sleep_until(net::io::clock::now() + 20ms);
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

    auto results = run(sched, server, fetch_all(&sched, &client, std::vector<std::string>(6, "127.0.0.1"), server.port));

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

    net::http::client client{&sched, {.connect_timeout = 100ms, .max_connections_per_host = 1}};

    auto results = run(sched, server, fetch_all(&sched, &client, {"127.0.0.1", "127.0.0.1"}, server.port));

    std::ranges::sort(results);
    CHECK(results
          == std::vector<std::string>{"error: " + std::make_error_condition(std::errc::timed_out).message(), "hello"});
}

TEST_CASE("idle connections are closed after idle_timeout", "[http][client][pool]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    keepalive_server   server{&sched};

    net::http::client client{&sched, {.idle_timeout = 100ms}};

    auto test = [](net::io::scheduler*  sched,
                   net::http::client*   client,
                   keepalive_server*    server) -> task<std::pair<net::http::connection_stats, int>>
    {
        co_await fetch(client, "127.0.0.1", server->port);
        co_await sched->sleep_until(net::io::clock::now() + 300ms);

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

            auto resp = co_await client->send(post);
            co_return first + (resp.has_value() ? "sent" : " failed");
        };

        CHECK(run(sched, server, test(&client, server.port)) == "hello failed");
        CHECK(server.accepted == 1);
    }
}
