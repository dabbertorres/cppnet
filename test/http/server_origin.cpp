// ORIGIN on the server: server_config::http2_origins, read as https origins (and normalized) when the server's made,
// and never sent over cleartext. What's sent over TLS, and the certificate's check, are test/tls/server.cpp's.
#include <algorithm>
#include <chrono>
#include <expected>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/h2_client.hpp"
#include "http/http2.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;

using namespace std::chrono_literals;
using net::coro::task;
using test::h2_client;

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(2); }

std::string repeat(std::string_view s, int n)
{
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

http::router make_router()
{
    http::router r;
    r.GET("/",
          [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          { co_await resp.send(http::status::NO_CONTENT, 0); });
    return r;
}

// make_server makes a cleartext server with origins.
http::server make_server(net::io::scheduler* sched, std::vector<std::string> origins)
{
    return {sched,
            net::listener::make(sched, "127.0.0.1", "0").value(),
            make_router(),
            http::server_config{.http2_origins = std::move(origins)}};
}

}

TEST_CASE("the server refuses http2_origins that aren't https origins", "[http][http2][server][origin]")
{
    const auto origin = GENERATE(as<std::string>{},
                                 "",
                                 "localhost",
                                 "http://localhost",
                                 "https://",
                                 "https://localhost/",
                                 "https://localhost/path",
                                 "https://localhost?query",
                                 "https://localhost#fragment",
                                 "https://user@localhost",
                                 "https://*.example.com",
                                 "https://-a.example",
                                 "https://a-.example",
                                 "https://a..example",
                                 "https://a.example.",
                                 "https://" + std::string(64, 'a') + ".example",
                                 "https://" + repeat("abcd.", 50) + "example",
                                 "https://under_score.example",
                                 "https://localhost:",
                                 "https://localhost:0",
                                 "https://localhost:65536",
                                 "https://localhost:123456",
                                 "https://localhost:-1",
                                 "https://localhost:80:80",
                                 "https://[::1",
                                 "https://[::1]x",
                                 "https://[::1]x8443",
                                 "https://[127.0.0.1]",
                                 "https://::1",
                                 "https://[not an address]");
    CAPTURE(origin);

    net::io::scheduler sched{make_pool()};
    CHECK_THROWS_AS(make_server(&sched, {"https://localhost", origin}), std::invalid_argument);
}

TEST_CASE("the server takes https origins, without TLS too", "[http][http2][server][origin]")
{
    const auto origin = GENERATE(as<std::string>{},
                                 "https://localhost",
                                 "HTTPS://LocalHost:8443",
                                 "https://a-b.example:443",
                                 "https://" + std::string(63, 'a') + ".example",
                                 "https://127.0.0.1",
                                 "https://[::1]:8443",
                                 "https://[2001:DB8::1]",
                                 "https://1.example");
    CAPTURE(origin);

    // (without TLS, nothing's checked against a certificate: none is sent anyway.)
    net::io::scheduler sched{make_pool()};
    CHECK_NOTHROW(make_server(&sched, {origin}));
}

TEST_CASE("a server without TLS sends no ORIGIN, whatever its http2_origins", "[http][http2][server][origin]")
{
    net::io::scheduler sched{make_pool()};
    auto               server = make_server(&sched, {"https://localhost", "https://127.0.0.1:8443"});
    REQUIRE(sched.spawn(server.serve()));

    // what the client saw: what went wrong, if anything, the response's status, the ORIGINs, and the frames' types.
    struct seen
    {
        std::string                           failed;
        std::string                           status;
        std::vector<std::vector<std::string>> origins;
        std::vector<http2::frame_type>        frame_types;
    };

    const auto out = sched.run_to_completion(
        [](net::io::scheduler* sched, std::string port) -> task<seen>
        {
            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
            seen out;
            if (!conn.has_value())
            {
                out.failed = "not connected: " + conn.error().message();
                co_return out;
            }
            h2_client c{&*conn, &*conn, &*conn};

            // by the time a request is answered, an ORIGIN would have come, after the server's SETTINGS.
            const auto id   = c.request("GET", "/");
            const bool done = co_await c.wait_for(id);
            out.status      = c.responses[id].status;
            out.origins     = c.origins;
            out.frame_types = c.frame_types;
            if (!done) out.failed = c.failure.empty() ? "not answered" : c.failure;
            co_return out;
        }(&sched, std::to_string(server.local_port())));

    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());
    CHECK(out.status == "204");
    CHECK(out.origins.empty());

    const auto& frames = out.frame_types;
    REQUIRE_FALSE(frames.empty());
    CHECK(frames.front() == http2::frame_type::settings);
    CHECK(std::ranges::find(frames, http2::frame_type::origin) == frames.end());
}
