#include "http/client.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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
