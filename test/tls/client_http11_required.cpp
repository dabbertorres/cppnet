// The HTTP client's fallback to HTTP/1.1 over TLS, when an HTTP/2 server asks for it with HTTP_1_1_REQUIRED (RFC 9113,
// section 7): the request's sent again on a connection whose handshake offered only http/1.1 by ALPN, so the server
// can't choose HTTP/2 again.
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/connection.hpp"
#include "http/connection_pool.hpp"
#include "http/h2_client.hpp"
#include "http/http2.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "listen.hpp"
#include "tls/test_pki.hpp"
#include "tls/tls.hpp"
#include "url.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;
namespace tls   = net::tls;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::pki;
using test::server_with;

namespace
{

// fetched is what a request sent in the background came to: "<HTTP major version> <status> <body>", or the error.
struct fetched
{
    std::atomic<std::uint64_t> finished{0};
    std::string                line;

    // wait waits (up to 10 seconds) for the request to have finished, returning whether it has.
    task<bool> wait(net::io::scheduler* sched)
    {
        const auto deadline = net::io::clock::now() + 10s;
        while (finished.load() == 0)
        {
            if (co_await sched->wait_on(finished, 0, deadline)) break;
        }
        co_return finished.load() != 0;
    }
};

// get GETs url, noting what it came to in out; then shuts stop down, so that an accept for a connection the client
// didn't make fails, rather than waiting for ever.
task<> get(net::io::scheduler* sched, http::client* c, std::string url, fetched* out, net::listener* stop)
{
    auto resp = co_await c->get(url);
    if (!resp.has_value())
    {
        out->line = "error: " + resp.error().message();
    }
    else
    {
        const auto body = co_await net::io::read_all(*resp->body);
        out->line       = std::format("{} {} {}",
                                      resp->version.major,
                                      static_cast<int>(resp->status_code),
                                      body.value_or("(failed: " + body.error_or(std::error_code{}).message() + ")"));
    }

    out->finished.store(1);
    sched->notify_all(out->finished);
    stop->shutdown();
}

// accept accepts a connection on listener, and runs a server's handshake over it, offering h2 and http/1.1 by ALPN.
task<std::optional<tls::stream>> accept(const net::listener& listener)
{
    auto accepted = co_await listener.accept();
    if (!accepted.has_value()) co_return std::nullopt;

    auto stream = co_await tls::stream::accept(std::move(*accepted),
                                               server_with(pki().names_chain, pki().names_key),
                                               net::io::clock::now() + 5s,
                                               {
                                                   .alpn = std::vector<std::string>{"h2", "http/1.1"}
    });
    if (!stream.has_value()) co_return std::nullopt;
    co_return std::move(*stream);
}

// read_head reads a request's head from stream (without the empty line that ends it), or nothing if it ends first.
task<std::optional<std::string>> read_head(tls::stream& stream)
{
    std::string                  pending;
    std::array<std::byte, 1'024> buf{};
    while (!pending.contains("\r\n\r\n"))
    {
        const auto res = co_await stream.read(buf);
        pending.append(reinterpret_cast<const char*>(buf.data()), res.count);
        if (res.err) co_return std::nullopt;
    }
    co_return pending.substr(0, pending.find("\r\n\r\n"));
}

}

TEST_CASE("over TLS, a request reset with HTTP_1_1_REQUIRED goes again on a connection offering only http/1.1",
          "[tls][http][client][http2][http11-required]")
{
    fetched            out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    http::client_options options;
    options.tls.trusted_ca_file     = pki().ca;
    options.response_header_timeout = 10s;
    http::client client{&sched, options};

    const auto test = [&] -> task<>
    {
        const auto url = std::format("https://127.0.0.1:{}/x", listener.local_port());
        REQUIRE(sched.spawn(get(&sched, &client, url, &out, &listener)));

        // HTTP/2 first, as ALPN agrees.
        auto first = co_await accept(listener);
        REQUIRE(first.has_value());
        CHECK(first->alpn() == "h2");

        http::detail::connection side{std::move(*first)};
        test::h2_client          played{&http::detail::as_reader(side),
                                        &http::detail::as_writer(side),
                                        &http::detail::socket_of(side),
                                        http2::role::server};
        bool                     got = co_await played.pump([&] { return played.responses.contains(1); });
        REQUIRE(got);
        played.s.reset(1, http2::error_code::http_1_1_required);
        got = co_await played.pump([] { return true; });
        REQUIRE(got);

        // then a connection whose client offered only http/1.1, though the server offers h2 first.
        auto second = co_await accept(listener);
        REQUIRE(second.has_value());
        CHECK(second->alpn() == "http/1.1");

        const auto head = co_await read_head(*second);
        REQUIRE(head.has_value());
        CHECK(head->starts_with("GET /x HTTP/1.1\r\n"));
        const auto res = co_await net::io::write_all(*second, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"sv);
        REQUIRE_FALSE(res.err);

        got = co_await out.wait(&sched);
        REQUIRE(got);
    };

    sched.run_to_completion(test());
    CHECK(out.line == "1 200 ok");
}
