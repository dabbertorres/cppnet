#include "http/server.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception> // IWYU pragma: keep
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "url.hpp"

namespace http = net::http;

namespace
{

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
