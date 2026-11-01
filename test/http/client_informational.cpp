#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
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
#include "http/client.hpp"
#include "http/connection_pool.hpp"
#include "http/h2_client.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http.hpp"
#include "http/http2.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "url.hpp"

// The client's interim (1xx) responses (client_request::on_informational), over HTTP/1.1 and HTTP/2.
namespace
{

namespace http  = net::http;
namespace http2 = net::http::http2;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;

constexpr auto style = "</style.css>; rel=preload"sv;
constexpr auto app   = "</app.js>; rel=preload"sv;

// interim is an informational response on_informational was called with: its status, and its Link fields.
struct interim
{
    int                      status = 0;
    std::vector<std::string> links;
};

// interims records what on_informational is called with. It's only called on the task sending the request, which
// reads it once send() has returned, so it needs no lock.
struct interims
{
    std::vector<interim> seen;

    // record records a head.
    void record(http::status code, const http::headers& fields)
    {
        auto& head  = seen.emplace_back();
        head.status = static_cast<int>(code);
        if (const auto links = fields.get_all("Link"sv); links.has_value())
        {
            for (const auto& link : *links) head.links.emplace_back(link);
        }
    }

    // callback is an on_informational that records what it's called with in this.
    std::function<void(http::status, const http::headers&)> callback()
    {
        return [this](http::status code, const http::headers& fields) { record(code, fields); };
    }
};

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// hints sends two Early Hints (the second with the first's Link too, as the response keeps its fields), then the final
// response, which keeps them.
task<void> hints(http::response_writer& resp)
{
    resp.headers().set("Link", style);
    co_await resp.send(http::status::EARLY_HINTS);
    resp.headers().add("Link", app);
    co_await resp.send(http::status::EARLY_HINTS);
}

// served is cppnet's server on 127.0.0.1, with routes for the tests.
struct served
{
    explicit served(net::io::scheduler* sched)
        : g{sched}
        , server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), routes(sched)}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    [[nodiscard]] std::string url(std::string_view path) const
    {
        return std::format("http://127.0.0.1:{}{}", server.local_port(), path);
    }

    http::router routes(net::io::scheduler* sched)
    {
        http::router r;

        // Early Hints, then the final response.
        r.GET("/hints",
              [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
              {
                  co_await hints(resp);
                  auto* body = co_await resp.send(http::status::OK, 4);
                  co_await net::io::write_all(*body, "done"sv);
              });

        // the same, but the final response waits for the test to open the gate.
        r.GET("/hints-gated",
              [this](const http::server_request& req, http::response_writer& resp) -> task<void>
              {
                  co_await hints(resp);
                  if (co_await g.pass(req.stop)) co_return;
                  auto* body = co_await resp.send(http::status::OK, 4);
                  co_await net::io::write_all(*body, "done"sv);
              });

        // Early Hints, then a redirect to /hints.
        r.GET("/hop",
              [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
              {
                  resp.headers().set("Link", "</hop.css>; rel=preload");
                  co_await resp.send(http::status::EARLY_HINTS);
                  resp.headers().set("Location", "/hints");
                  (void)co_await resp.send(http::status::FOUND, 0);
              });

        // Early Hints, then nothing until the request's stopped (by the client going), which it records.
        r.GET("/hints-then-wait",
              [this, sched](const http::server_request& req, http::response_writer& resp) -> task<void>
              {
                  resp.headers().set("Link", style);
                  co_await resp.send(http::status::EARLY_HINTS);
                  (void)co_await probe.wait(sched, req.stop);
              });

        // Early Hints every 50 ms, until the request's stopped (by the client going).
        r.GET("/hints-forever",
              [sched](const http::server_request& req, http::response_writer& resp) -> task<void>
              {
                  resp.headers().set("Link", style);
                  while (true)
                  {
                      co_await resp.send(http::status::EARLY_HINTS);
                      if (co_await sched->sleep_until(net::io::clock::now() + 50ms, req.stop)) co_return;
                  }
              });

        // the request's body, read (sending 100 Continue first, if the request expects it), and sent back.
        r.POST("/echo",
               [](http::server_request& req, http::response_writer& resp) -> task<void>
               {
                   auto body = co_await net::io::read_all(*req.body);
                   if (!body.has_value()) throw std::system_error{body.error()};

                   auto* out = co_await resp.send(http::status::OK, body->size());
                   co_await net::io::write_all(*out, *body);
               });

        return r;
    }

    test::gate       g; // holds /hints-gated's final response
    test::stop_probe probe;
    http::server     server;
};

http::client_options options_for(bool http2)
{
    http::client_options options;
    options.http2_prior_knowledge = http2;
    return options;
}

http::client_request get(const std::string& url) { return {.uri = net::url::parse(url).value()}; }

// outcome is what a request came to: its response's status, version and body, or the error.
struct outcome
{
    int             status  = 0;
    std::uint32_t   version = 0;
    std::string     body;
    std::string     link; // the final response's Link fields, joined by ", "
    std::error_code err;
};

task<outcome> send(http::client* client, http::client_request req)
{
    outcome out;

    auto resp = co_await client->send(std::move(req));
    if (!resp.has_value())
    {
        out.err = resp.error();
        co_return out;
    }

    out.status  = static_cast<int>(resp->status_code);
    out.version = resp->version.major;
    for (const auto link : resp->headers.get_list("Link"sv))
        out.link += (out.link.empty() ? "" : ", ") + std::string{link};

    auto body = co_await net::io::read_all(*resp->body);
    if (body.has_value()) out.body = std::move(*body);
    else out.err = body.error();
    co_return out;
}

}

TEST_CASE("on_informational sees each interim response, in order, as it comes", "[http][client][interim]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(http2)};

    // the final response waits until the callback has seen both Early Hints: so it's called as they come, before
    // send() returns (which a response that never came would time out).
    interims seen;
    auto     req         = get(server.url("/hints-gated"));
    req.on_informational = [&](http::status code, const http::headers& fields)
    {
        seen.record(code, fields);
        if (seen.seen.size() == 2) server.g.open();
    };

    const auto out = sched.run_to_completion(send(&client, std::move(req)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.version == (http2 ? 2U : 1U));
    CHECK(out.body == "done");
    CHECK(out.link == std::format("{}, {}", style, app)); // the final response keeps them

    REQUIRE(seen.seen.size() == 2);
    CHECK(seen.seen[0].status == 103);
    CHECK(seen.seen[0].links == std::vector<std::string>{std::string{style}});
    CHECK(seen.seen[1].status == 103);
    CHECK(seen.seen[1].links == std::vector<std::string>{std::string{style}, std::string{app}});
}

TEST_CASE("without on_informational, interim responses are skipped", "[http][client][interim]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(http2)};

    const auto out = sched.run_to_completion(send(&client, get(server.url("/hints"))));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.version == (http2 ? 2U : 1U));
    CHECK(out.body == "done");
    CHECK(out.link == std::format("{}, {}", style, app));
}

TEST_CASE("on_informational sees the server's own 100 Continue, if the request expects it", "[http][client][interim]")
{
    const bool http2  = GENERATE(false, true);
    const bool expect = GENERATE(false, true);
    CAPTURE(http2, expect);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(http2)};

    // the client doesn't send Expect by itself, so it's set by hand (over HTTP/1.1, it then waits for the 100).
    interims             seen;
    http::client_request req{.method = http::request_method::POST, .uri = net::url::parse(server.url("/echo")).value()};
    req.set_body("hello");
    if (expect) req.headers.set("Expect", "100-continue");
    req.on_informational = seen.callback();

    const auto out = sched.run_to_completion(send(&client, std::move(req)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.body == "hello");
    if (expect)
    {
        REQUIRE(seen.seen.size() == 1);
        CHECK(seen.seen[0].status == 100);
        CHECK(seen.seen[0].links.empty());
    }
    else
    {
        CHECK(seen.seen.empty());
    }
}

TEST_CASE("on_informational is kept for a redirect's request", "[http][client][interim][redirect]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(http2)};

    interims seen;
    auto     req         = get(server.url("/hop"));
    req.max_redirects    = 1;
    req.on_informational = seen.callback();

    const auto out = sched.run_to_completion(send(&client, std::move(req)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.body == "done");

    // the redirect's, then the next request's.
    REQUIRE(seen.seen.size() == 3);
    CHECK(seen.seen[0].links == std::vector<std::string>{"</hop.css>; rel=preload"});
    CHECK(seen.seen[1].links == std::vector<std::string>{std::string{style}});
    CHECK(seen.seen[2].links == std::vector<std::string>{std::string{style}, std::string{app}});
}

TEST_CASE("an exception from on_informational fails the request, abandoning its connection or stream",
          "[http][client][interim]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(http2)};

    struct result
    {
        std::string thrown;
        bool        stopped = false; // the server saw its client go
        std::size_t open    = 0;     // the client's connections then
        outcome     next;            // the next request
        std::size_t open_after = 0;  // and the client's connections after it
    };

    const auto test = [](net::io::scheduler* sched, http::client* client, served* server) -> task<result>
    {
        result out;

        auto req             = get(server->url("/hints-then-wait"));
        req.on_informational = [](http::status /*code*/, const http::headers& /*fields*/)
        { throw std::runtime_error{"enough"}; };

        try
        {
            (void)co_await client->send(std::move(req));
        }
        catch (const std::runtime_error& e)
        {
            out.thrown = e.what();
        }

        // the server sees its client go: the connection closed (HTTP/1.1), or the stream reset (HTTP/2).
        out.stopped = co_await test::stop_probe::reach(sched, server->probe.woke, 1);
        out.open    = client->stats().open;

        // and the pool's sound: the next request is answered.
        out.next       = co_await send(client, get(server->url("/hints")));
        out.open_after = client->stats().open;
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, &client, &server));
    CHECK(out.thrown == "enough");
    CHECK(out.stopped);
    CHECK(server.probe.canceled.load() == 1);

    // HTTP/1.1's connection is gone; HTTP/2's is kept, for the next request.
    CHECK(out.open == (http2 ? 1U : 0U));

    CAPTURE(out.next.err.message());
    CHECK(out.next.status == 200);
    CHECK(out.next.body == "done");
    CHECK(out.open_after == 1);
}

TEST_CASE("an interim response doesn't put off response_header_timeout", "[http][client][interim][timeout]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};

    auto options                    = options_for(http2);
    options.response_header_timeout = 300ms;
    http::client client{&sched, options};

    interims seen;
    auto     req         = get(server.url("/hints-forever"));
    req.on_informational = seen.callback();

    const auto start   = net::io::clock::now();
    const auto out     = sched.run_to_completion(send(&client, std::move(req)));
    const auto elapsed = net::io::clock::now() - start;

    CAPTURE(out.err.message(), seen.seen.size());
    CHECK(out.err == std::errc::timed_out);
    CHECK(elapsed >= 300ms);
    CHECK(elapsed < 3s);
    CHECK(seen.seen.size() >= 2);
}

namespace
{

// raw_server accepts connections on 127.0.0.1, answering each request as its test says, from the raw bytes.
struct raw_server
{
    explicit raw_server(net::io::scheduler* sched)
        : listener{net::listener::make(sched, "127.0.0.1", "0").value()}
    {}

    [[nodiscard]] std::string url(std::string_view path) const
    {
        return std::format("http://127.0.0.1:{}{}", listener.local_port(), path);
    }

    // read_head reads a request's head (the tests' have no bodies), returning false if the connection ends first.
    static task<bool> read_head(net::tcp_socket& conn, std::string& pending)
    {
        std::array<std::byte, 1'024> buf{};
        while (!pending.contains("\r\n\r\n"))
        {
            auto res = co_await conn.read(buf);
            pending.append(reinterpret_cast<const char*>(buf.data()), res.count);
            if (res.err) co_return false;
        }
        pending.erase(0, pending.find("\r\n\r\n") + 4);
        co_return true;
    }

    // drain reads until the client closes the connection.
    static task<> drain(net::tcp_socket& conn)
    {
        std::array<std::byte, 1'024> buf{};
        while (!(co_await conn.read(buf)).err)
        {}
    }

    net::listener    listener;
    std::atomic<int> accepted{0};
};

// big_hint is an Early Hints head of about 100 KB (its Link's value is 100000 bytes).
std::string big_hint() { return "HTTP/1.1 103 Early Hints\r\nLink: " + std::string(100'000, 'x') + "\r\n\r\n"; }

// hint_past_limit answers a request with eleven Early Hints of 100 KB, more than the client takes of a response's
// heads (1 MiB), and nothing more.
task<> hint_past_limit(raw_server* server)
{
    auto conn = co_await server->listener.accept();
    if (!conn.has_value()) co_return;
    ++server->accepted;

    std::string pending;
    if (!co_await raw_server::read_head(*conn, pending)) co_return;

    std::string heads;
    for (int i = 0; i < 11; ++i) heads += big_hint();
    if ((co_await net::io::write_all(*conn, heads)).err) co_return;
    co_await raw_server::drain(*conn);
}

// hint_then_close answers a connection's first request, and to its second, sends Early Hints and closes it.
task<> hint_then_close(raw_server* server)
{
    while (true)
    {
        auto conn = co_await server->listener.accept();
        if (!conn.has_value()) co_return;
        ++server->accepted;

        std::string pending;
        if (!co_await raw_server::read_head(*conn, pending)) continue;
        if ((co_await net::io::write_all(*conn, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"sv)).err) continue;

        if (!co_await raw_server::read_head(*conn, pending)) continue;
        (void)co_await net::io::write_all(*conn, "HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n"sv);
    }
}

}

TEST_CASE("an HTTP/1.1 response's interim heads past the limit fail it", "[http][client][interim][limits]")
{
    const bool callback = GENERATE(false, true);
    CAPTURE(callback);

    net::io::scheduler sched{make_pool()};
    raw_server         server{&sched};
    http::client       client{&sched};
    REQUIRE(sched.spawn(hint_past_limit(&server)));

    interims seen;
    auto     req = get(server.url("/"));
    if (callback) req.on_informational = seen.callback();

    const auto out = sched.run_to_completion(send(&client, std::move(req)));

    CHECK(out.err == std::errc::value_too_large);

    // all of a response's heads count against the limit together: ten of them (1000360 bytes) fit in 1 MiB, the
    // eleventh doesn't.
    CHECK(seen.seen.size() == (callback ? 10U : 0U));
    CHECK(client.stats() == http::connection_stats{});
}

TEST_CASE("an HTTP/1.1 request isn't sent again once an interim response has come", "[http][client][interim][retry]")
{
    net::io::scheduler sched{make_pool()};
    raw_server         server{&sched};
    http::client       client{&sched};
    REQUIRE(sched.spawn(hint_then_close(&server)));

    const auto test = [](http::client* client, raw_server* server) -> task<>
    {
        const auto first = co_await send(client, get(server->url("/")));
        CHECK(first.body == "hello");

        // the second request, on the reused connection, gets Early Hints, then the connection ends: the server had
        // the request, so it's not sent again (as it would be if the connection had ended with nothing).
        interims seen;
        auto     req         = get(server->url("/"));
        req.on_informational = seen.callback();

        const auto second = co_await send(client, std::move(req));
        CAPTURE(second.err.message());
        CHECK(second.err == net::io::status_condition::closed);
        CHECK(seen.seen.size() == 1);
        CHECK(server->accepted.load() == 1);
    };

    sched.run_to_completion(test(&client, &server));
}

namespace
{

// past_limit_hints sends eleven Early Hints of about 100 KB each (their Link's value is 100000 bytes) on stream 1:
// more than the client takes of a response's informational heads altogether (1 MiB, and 32 bytes a field). It returns
// whether they were all submitted.
bool past_limit_hints(test::h2_client& server)
{
    const std::string                     link(100'000, 'x');
    const std::vector<http::hpack::field> head{
        {.name = ":status", .value = "103"},
        {   .name = "link",  .value = link}
    };
    for (int i = 0; i < 11; ++i)
    {
        if (server.s.submit_headers(1, head, false)) return false;
    }
    return true;
}

// sent is a request sent in the background: what its callback saw, and what it came to. Declared before the
// scheduler, as its task uses it.
struct sent
{
    interims                   seen;
    outcome                    out;
    std::atomic<std::uint64_t> finished{0};
};

task<> send_in_background(net::io::scheduler* sched, http::client* client, http::client_request req, sent* to)
{
    to->out = co_await send(client, std::move(req));
    to->finished.store(1);
    sched->notify_all(to->finished);
}

}

TEST_CASE("an HTTP/2 response's interim heads past the limit fail it, resetting its stream",
          "[http][http2][client][interim][limits]")
{
    const bool callback = GENERATE(false, true);
    CAPTURE(callback);

    sent               request;
    net::io::scheduler sched{make_pool()};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();
    http::client       client{&sched, options_for(true)};

    // what went wrong with the test's own steps, if anything.
    const auto test = [](net::io::scheduler* sched, net::listener* listener, sent* request) -> task<std::string>
    {
        auto accepted = co_await listener->accept();
        if (!accepted.has_value()) co_return "no connection";
        test::h2_client server{&*accepted, &*accepted, &*accepted, http2::role::server};

        const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
        if (!requested) co_return "no request";
        if (!past_limit_hints(server)) co_return "the hints weren't submitted";

        // the client resets the stream alone, keeping the connection (open after the reset, as a PING round trip
        // shows).
        const bool reset = co_await server.pump([&] { return static_cast<bool>(server.responses[1].reset); });
        if (!reset) co_return "not reset";
        if (server.responses[1].reset != http2::error_code::protocol_error)
            co_return "reset: " + server.responses[1].reset.message();
        const bool synced = co_await server.sync();
        if (!synced || server.closed) co_return "the connection ended";

        const bool done = co_await test::stop_probe::reach(sched, request->finished, 1);
        if (!done) co_return "the request didn't finish";
        co_return std::string{};
    };

    // the client's request, against a server the test plays.
    auto req = get(std::format("http://127.0.0.1:{}/", listener.local_port()));
    if (callback) req.on_informational = request.seen.callback();
    const bool spawned = sched.spawn(send_in_background(&sched, &client, std::move(req), &request));
    REQUIRE(spawned);

    const auto failed = sched.run_to_completion(test(&sched, &listener, &request));
    CAPTURE(failed);
    CHECK(failed.empty());
    CHECK(request.out.err == std::errc::value_too_large);

    // the heads that came before the limit are passed on: ten of them (by HTTP/2's count, 100078 bytes each, 1000780
    // together) fit in the client's 1 MiB and 3200 bytes, the eleventh doesn't.
    CHECK(request.seen.seen.size() == (callback ? 10U : 0U));
}
