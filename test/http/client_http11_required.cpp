#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
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
#include "http/connection.hpp"
#include "http/connection_pool.hpp"
#include "http/h2_client.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http.hpp"
#include "http/http2.hpp"
#include "http/http2_client.hpp"
#include "http/http2_config.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/string_reader.hpp"
#include "io/util.hpp"
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

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// prior_knowledge is the options the tests' clients have: HTTP/2 in cleartext, and a head that never comes failing
// the request rather than hanging the test.
http::client_options prior_knowledge()
{
    http::client_options options;
    options.http2_prior_knowledge   = true;
    options.response_header_timeout = 10s;
    return options;
}

// fake_host is a host that the client connects to, whose connections the test accepts, to play the server on.
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

// watch shuts the listeners down if the test hasn't finished (done isn't set) within 20 seconds, so an accept for a
// connection the client never makes fails rather than hangs. The scheduler's stopping ends it.
task<> watch(net::io::scheduler* sched, const std::atomic<std::uint64_t>* done, std::vector<net::listener*> listeners)
{
    if (co_await sched->wait_on(*done, 0, net::io::clock::now() + 20s) != std::errc::timed_out) co_return;
    for (auto* l : listeners) l->shutdown();
}

// played is an HTTP/2 connection the test accepted, and the server it plays on it (with test::h2_client).
struct played
{
    explicit played(net::tcp_socket accepted)
        : socket{std::move(accepted)}
        , server{&socket, &socket, &socket, http2::role::server}
    {}

    net::tcp_socket socket;
    test::h2_client server;
};

task<std::unique_ptr<played>> accept_h2(fake_host* host)
{
    auto accepted = co_await host->listener.accept();
    REQUIRE(accepted.has_value());
    co_return std::make_unique<played>(std::move(*accepted));
}

// field is a request's field (or pseudo-header) as the server it played received it.
std::string field(const test::h2_client::response& r, std::string_view name)
{
    for (const auto& [n, v] : r.headers)
    {
        if (n == name) return v;
    }
    return {};
}

// h1_peer is an HTTP/1.1 connection the test accepted, to play the server on, from the raw bytes.
struct h1_peer
{
    net::tcp_socket socket;
    std::string     pending; // read past the last request

    // read_request reads a request's head (without the empty line that ends it) and its body (as long as its
    // Content-Length says), or nothing if the connection ends first, or nothing comes for 10 seconds.
    task<std::optional<std::pair<std::string, std::string>>> read_request()
    {
        socket.set_read_deadline(net::io::clock::now() + 10s);

        std::array<std::byte, 1'024> buf{};
        const auto                   more = [&] -> task<bool>
        {
            auto res = co_await socket.read(buf);
            pending.append(reinterpret_cast<const char*>(buf.data()), res.count);
            co_return !res.err;
        };

        while (!pending.contains("\r\n\r\n"))
        {
            if (!co_await more()) co_return std::nullopt;
        }

        const auto end  = pending.find("\r\n\r\n");
        auto       head = pending.substr(0, end);
        pending.erase(0, end + 4);

        std::size_t length = 0;
        if (const auto at = head.find("\r\nContent-Length: "); at != std::string::npos)
            length = std::stoul(head.substr(at + 18, head.find("\r\n", at + 2) - at - 18));

        while (pending.size() < length)
        {
            if (!co_await more()) co_return std::nullopt;
        }

        auto body = pending.substr(0, length);
        pending.erase(0, length);
        co_return std::pair{std::move(head), std::move(body)};
    }

    task<bool> send(std::string_view bytes) { co_return !(co_await net::io::write_all(socket, bytes)).err; }
};

task<std::optional<h1_peer>> accept_h1(const net::listener& listener)
{
    auto accepted = co_await listener.accept();
    if (!accepted.has_value()) co_return std::nullopt;
    co_return h1_peer{.socket = std::move(*accepted), .pending = {}};
}

// outcome is what a request sent in the background came to: its response's status and body, or the error.
struct outcome
{
    std::atomic<std::uint64_t> finished{0};
    int                        status = 0;
    std::string                body;
    std::error_code            err;

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

// send_with sends req with client, recording what it came to in out (reading the response's body).
task<> send_with(net::io::scheduler* sched, http::client* client, http::client_request req, outcome* out)
{
    auto resp = co_await client->send(std::move(req));
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

http::client_request request(http::request_method method, const std::string& url)
{
    return {.method = method, .uri = net::url::parse(url).value()};
}

const auto http11_required = http2::error_code::http_1_1_required;

}

TEST_CASE("a request the server resets with HTTP_1_1_REQUIRED goes over HTTP/1.1, and the HTTP/2 connection goes on",
          "[http][http2][client][http11-required]")
{
    std::atomic<std::uint64_t> done{0};
    // used by the requests' tasks, which a failing REQUIRE may leave to the scheduler to end.
    outcome            first;
    outcome            second;
    outcome            third;
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};
    REQUIRE(sched.spawn(watch(&sched, &done, {&host.listener})));

    const auto test = [&] -> task<>
    {
        const net::util::defer finished{[&] noexcept { test::stop_probe::bump(&sched, done); }};

        REQUIRE(sched.spawn(send_with(&sched, &client, request(http::request_method::GET, host.url("/a")), &first)));

        auto h2  = co_await accept_h2(&host);
        bool got = co_await h2->server.pump([&] { return h2->server.responses.contains(1); });
        REQUIRE(got);
        h2->server.s.reset(1, http11_required);
        got = co_await h2->server.pump([] { return true; }); // (just written)
        REQUIRE(got);

        // sent again on a connection of its own, in HTTP/1.1 from its first byte (no preface).
        auto h1 = co_await accept_h1(host.listener);
        REQUIRE(h1.has_value());
        auto req = co_await h1->read_request();
        REQUIRE(req.has_value());
        CHECK(req->first.starts_with("GET /a HTTP/1.1\r\n"));
        got = co_await h1->send("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none"sv);
        REQUIRE(got);

        got = co_await first.wait(&sched);
        REQUIRE(got);
        CHECK(first.err == std::error_code{});
        CHECK(first.body == "one");

        // both connections are kept: the next request goes on the HTTP/2 one.
        CHECK(client.stats() == http::connection_stats{.open = 2, .idle = 2});

        REQUIRE(sched.spawn(send_with(&sched, &client, request(http::request_method::GET, host.url("/b")), &second)));
        got = co_await h2->server.pump([&] { return h2->server.responses.contains(3); });
        REQUIRE(got);
        h2->server.respond(3, "200", "two");
        got = co_await h2->server.pump([] { return true; });
        REQUIRE(got);

        got = co_await second.wait(&sched);
        REQUIRE(got);
        CHECK(second.body == "two");

        // one the server asks for HTTP/1.1 again takes the idle HTTP/1.1 connection, rather than making another.
        REQUIRE(sched.spawn(send_with(&sched, &client, request(http::request_method::GET, host.url("/c")), &third)));
        got = co_await h2->server.pump([&] { return h2->server.responses.contains(5); });
        REQUIRE(got);
        h2->server.s.reset(5, http11_required);
        got = co_await h2->server.pump([] { return true; });
        REQUIRE(got);

        req = co_await h1->read_request();
        REQUIRE(req.has_value());
        CHECK(req->first.starts_with("GET /c HTTP/1.1\r\n"));
        got = co_await h1->send("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nthree"sv);
        REQUIRE(got);

        got = co_await third.wait(&sched);
        REQUIRE(got);
        CHECK(third.body == "three");
        CHECK(client.stats().open == 2);
    };

    sched.run_to_completion(test());
}

TEST_CASE("a request reset with HTTP_1_1_REQUIRED goes over HTTP/1.1 if it's safe to repeat, and its body can be",
          "[http][http2][client][http11-required]")
{
    enum class body_kind : std::uint8_t
    {
        none,
        rewindable, // set_body(): get_body reads it again
        once,       // without get_body
    };

    struct test_case
    {
        http::request_method method;
        body_kind            body;
        bool                 again;
    };

    const auto tc =
        GENERATE(test_case{.method = http::request_method::GET, .body = body_kind::none, .again = true},
                 test_case{.method = http::request_method::DELETE, .body = body_kind::none, .again = true},
                 test_case{.method = http::request_method::PUT, .body = body_kind::rewindable, .again = true},
                 test_case{.method = http::request_method::PUT, .body = body_kind::once, .again = false},
                 test_case{.method = http::request_method::POST, .body = body_kind::none, .again = false},
                 test_case{.method = http::request_method::POST, .body = body_kind::rewindable, .again = false});
    CAPTURE(tc.method, static_cast<int>(tc.body));

    std::atomic<std::uint64_t> done{0};
    // used by the requests' tasks, which a failing REQUIRE may leave to the scheduler to end.
    outcome            out;
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};
    REQUIRE(sched.spawn(watch(&sched, &done, {&host.listener})));

    const auto test = [&] -> task<>
    {
        const net::util::defer finished{[&] noexcept { test::stop_probe::bump(&sched, done); }};

        auto req = request(tc.method, host.url("/r"));
        if (tc.body == body_kind::rewindable)
        {
            req.set_body("payload");
        }
        else if (tc.body == body_kind::once)
        {
            req.headers.set_content_length(7);
            req.body = std::make_unique<net::io::owned_string_reader>(std::string{"payload"});
        }

        const bool spawned = sched.spawn(send_with(&sched, &client, std::move(req), &out));
        REQUIRE(spawned);

        // reset once the request (and its body, if it has one) has all come.
        auto h2  = co_await accept_h2(&host);
        bool got = co_await h2->server.pump(
            [&]
            {
                const auto it = h2->server.responses.find(1);
                return it != h2->server.responses.end() && (tc.body == body_kind::none || it->second.done);
            });
        REQUIRE(got);
        h2->server.s.reset(1, http11_required);
        got = co_await h2->server.pump([] { return true; });
        REQUIRE(got);

        if (tc.again)
        {
            auto h1 = co_await accept_h1(host.listener);
            REQUIRE(h1.has_value());
            auto sent = co_await h1->read_request();
            REQUIRE(sent.has_value());
            CHECK(sent->first.starts_with(std::format("{} /r HTTP/1.1\r\n", http::method_string(tc.method))));
            CHECK(sent->second == (tc.body == body_kind::none ? "" : "payload"));
            got = co_await h1->send("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"sv);
            REQUIRE(got);
        }

        got = co_await out.wait(&sched);
        REQUIRE(got);
        if (tc.again)
        {
            CHECK(out.err == std::error_code{});
            CHECK(out.body == "ok");
        }
        else
        {
            // the server may have processed it, or its body can't be read again: the caller's told why.
            CHECK(out.err == http11_required);
            CHECK(client.stats().open == 1);
        }
    };

    sched.run_to_completion(test());
}

TEST_CASE("a request reset with HTTP_1_1_REQUIRED after an interim response isn't sent again",
          "[http][http2][client][http11-required]")
{
    std::atomic<std::uint64_t> done{0};
    // used by the requests' tasks, which a failing REQUIRE may leave to the scheduler to end.
    std::atomic<int>   interims{0};
    outcome            out;
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};
    REQUIRE(sched.spawn(watch(&sched, &done, {&host.listener})));

    const auto test = [&] -> task<>
    {
        const net::util::defer finished{[&] noexcept { test::stop_probe::bump(&sched, done); }};

        auto req             = request(http::request_method::GET, host.url("/"));
        req.on_informational = [&](http::status, const http::headers&) { interims.fetch_add(1); };

        const bool spawned = sched.spawn(send_with(&sched, &client, std::move(req), &out));
        REQUIRE(spawned);

        // the server's had the request, as its 103 says.
        auto h2  = co_await accept_h2(&host);
        bool got = co_await h2->server.pump([&] { return h2->server.responses.contains(1); });
        REQUIRE(got);
        h2->server.respond(1, "103", {}, false);
        got = co_await h2->server.sync();
        REQUIRE(got);
        h2->server.s.reset(1, http11_required);
        got = co_await h2->server.pump([] { return true; });
        REQUIRE(got);

        got = co_await out.wait(&sched);
        REQUIRE(got);
        CHECK(out.err == http11_required);
        CHECK(interims.load() == 1);
        CHECK(client.stats().open == 1);
    };

    sched.run_to_completion(test());
}

TEST_CASE("a response reset with HTTP_1_1_REQUIRED after its head fails its body's read, and isn't sent again",
          "[http][http2][client][http11-required]")
{
    std::atomic<std::uint64_t> done{0};
    // used by the requests' tasks, which a failing REQUIRE may leave to the scheduler to end.
    outcome            out;
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};
    REQUIRE(sched.spawn(watch(&sched, &done, {&host.listener})));

    const auto test = [&] -> task<>
    {
        const net::util::defer finished{[&] noexcept { test::stop_probe::bump(&sched, done); }};

        REQUIRE(sched.spawn(send_with(&sched, &client, request(http::request_method::GET, host.url("/")), &out)));

        auto h2  = co_await accept_h2(&host);
        bool got = co_await h2->server.pump([&] { return h2->server.responses.contains(1); });
        REQUIRE(got);
        h2->server.respond(1, "200", "part", false);
        got = co_await h2->server.sync();
        REQUIRE(got);
        h2->server.s.reset(1, http11_required);
        got = co_await h2->server.pump([] { return true; });
        REQUIRE(got);

        got = co_await out.wait(&sched);
        REQUIRE(got);
        CHECK(out.status == 200);
        CHECK(out.err == http11_required);
        CHECK(client.stats().open == 1);
    };

    sched.run_to_completion(test());
}

TEST_CASE("after GOAWAY with HTTP_1_1_REQUIRED, the streams after its last go over HTTP/1.1, and the others finish",
          "[http][http2][client][http11-required]")
{
    std::atomic<std::uint64_t> done{0};
    // used by the requests' tasks, which a failing REQUIRE may leave to the scheduler to end.
    outcome            one;
    outcome            three;
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};
    REQUIRE(sched.spawn(watch(&sched, &done, {&host.listener})));

    const auto test = [&] -> task<>
    {
        const net::util::defer finished{[&] noexcept { test::stop_probe::bump(&sched, done); }};

        REQUIRE(sched.spawn(send_with(&sched, &client, request(http::request_method::GET, host.url("/one")), &one)));
        auto h2  = co_await accept_h2(&host);
        bool got = co_await h2->server.pump([&] { return h2->server.responses.contains(1); });
        REQUIRE(got);

        // a POST, which the server didn't process, so it's sent again whatever its method, with its body read again.
        auto post = request(http::request_method::POST, host.url("/three"));
        post.set_body("posted");
        const bool spawned = sched.spawn(send_with(&sched, &client, std::move(post), &three));
        REQUIRE(spawned);
        got =
            co_await h2->server.pump([&] { return h2->server.responses.contains(3) && h2->server.responses[3].done; });
        REQUIRE(got);

        h2->server.s.goaway(1, http11_required);
        got = co_await h2->server.pump([] { return true; });
        REQUIRE(got);

        auto h1 = co_await accept_h1(host.listener);
        REQUIRE(h1.has_value());
        auto req = co_await h1->read_request();
        REQUIRE(req.has_value());
        CHECK(req->first.starts_with("POST /three HTTP/1.1\r\n"));
        CHECK(req->second == "posted");
        got = co_await h1->send("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nthree"sv);
        REQUIRE(got);

        got = co_await three.wait(&sched);
        REQUIRE(got);
        CHECK(three.err == std::error_code{});
        CHECK(three.body == "three");

        // the stream at the last is answered as it was going to be.
        h2->server.respond(1, "200", "one");
        got = co_await h2->server.pump([] { return true; });
        REQUIRE(got);
        got = co_await one.wait(&sched);
        REQUIRE(got);
        CHECK(one.err == std::error_code{});
        CHECK(one.body == "one");

        // and with that, the HTTP/2 connection's done.
        got = co_await h2->server.pump([&] { return h2->server.closed; });
        CHECK(got);
    };

    sched.run_to_completion(test());
}

TEST_CASE("after GOAWAY with HTTP_1_1_REQUIRED, a stream the connection's end cuts off goes over HTTP/1.1 if it's safe "
          "to repeat",
          "[http][http2][client][http11-required]")
{
    const auto method = GENERATE(http::request_method::GET, http::request_method::POST);
    CAPTURE(method);

    std::atomic<std::uint64_t> done{0};
    // used by the requests' tasks, which a failing REQUIRE may leave to the scheduler to end.
    outcome            out;
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};
    REQUIRE(sched.spawn(watch(&sched, &done, {&host.listener})));

    const auto test = [&] -> task<>
    {
        const net::util::defer finished{[&] noexcept { test::stop_probe::bump(&sched, done); }};

        auto req = request(method, host.url("/"));
        if (method == http::request_method::POST) req.set_body("posted");

        const bool spawned = sched.spawn(send_with(&sched, &client, std::move(req), &out));
        REQUIRE(spawned);

        // the stream's the last the server may have processed; then the connection ends without its response.
        {
            auto h2  = co_await accept_h2(&host);
            bool got = co_await h2->server.pump(
                [&] { return h2->server.responses.contains(1) && h2->server.responses[1].done; });
            REQUIRE(got);
            h2->server.s.goaway(1, http11_required);
            got = co_await h2->server.sync();
            REQUIRE(got);
            h2->socket.shutdown_write();
            (void)co_await h2->server.pump([&] { return h2->server.closed; });
        }

        if (method == http::request_method::GET)
        {
            auto h1 = co_await accept_h1(host.listener);
            REQUIRE(h1.has_value());
            auto sent = co_await h1->read_request();
            REQUIRE(sent.has_value());
            CHECK(sent->first.starts_with("GET / HTTP/1.1\r\n"));
            const bool got = co_await h1->send("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"sv);
            REQUIRE(got);
        }

        const bool got = co_await out.wait(&sched);
        REQUIRE(got);
        if (method == http::request_method::GET)
        {
            CHECK(out.err == std::error_code{});
            CHECK(out.body == "ok");
        }
        else
        {
            // it may have been processed: the caller's told the server asked for HTTP/1.1, rather than that the
            // connection was cut off.
            CHECK(out.err == http11_required);
        }
    };

    sched.run_to_completion(test());
}

TEST_CASE("a request refused, then reset with HTTP_1_1_REQUIRED on its second HTTP/2 connection, goes over HTTP/1.1",
          "[http][http2][client][http11-required]")
{
    std::atomic<std::uint64_t> done{0};
    // used by the requests' tasks, which a failing REQUIRE may leave to the scheduler to end.
    outcome            out;
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};
    REQUIRE(sched.spawn(watch(&sched, &done, {&host.listener})));

    const auto test = [&] -> task<>
    {
        const net::util::defer finished{[&] noexcept { test::stop_probe::bump(&sched, done); }};

        REQUIRE(sched.spawn(send_with(&sched, &client, request(http::request_method::GET, host.url("/")), &out)));

        auto first = co_await accept_h2(&host);
        bool got   = co_await first->server.pump([&] { return first->server.responses.contains(1); });
        REQUIRE(got);
        first->server.s.reset(1, http2::error_code::refused_stream);
        got = co_await first->server.pump([] { return true; });
        REQUIRE(got);

        // the retry (not a first attempt) is asked for HTTP/1.1.
        auto second = co_await accept_h2(&host);
        got         = co_await second->server.pump([&] { return second->server.responses.contains(1); });
        REQUIRE(got);
        second->server.s.reset(1, http11_required);
        got = co_await second->server.pump([] { return true; });
        REQUIRE(got);

        auto h1 = co_await accept_h1(host.listener);
        REQUIRE(h1.has_value());
        auto req = co_await h1->read_request();
        REQUIRE(req.has_value());
        CHECK(req->first.starts_with("GET / HTTP/1.1\r\n"));
        got = co_await h1->send("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"sv);
        REQUIRE(got);

        got = co_await out.wait(&sched);
        REQUIRE(got);
        CHECK(out.err == std::error_code{});
        CHECK(out.body == "ok");
        CHECK(client.stats().open == 3);
    };

    sched.run_to_completion(test());
}

TEST_CASE("a request whose HTTP/1.1 attempt fails, after HTTP_1_1_REQUIRED, fails with that attempt's error",
          "[http][http2][client][http11-required]")
{
    const bool cancel = GENERATE(false, true);
    CAPTURE(cancel);

    std::atomic<std::uint64_t> done{0};
    // used by the requests' tasks, which a failing REQUIRE may leave to the scheduler to end.
    std::stop_source   stop;
    outcome            out;
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    http::client       client{&sched, prior_knowledge()};
    REQUIRE(sched.spawn(watch(&sched, &done, {&host.listener})));

    const auto test = [&] -> task<>
    {
        const net::util::defer finished{[&] noexcept { test::stop_probe::bump(&sched, done); }};

        auto req = request(http::request_method::GET, host.url("/"));
        req.stop = stop.get_token();

        const bool spawned = sched.spawn(send_with(&sched, &client, std::move(req), &out));
        REQUIRE(spawned);

        auto h2  = co_await accept_h2(&host);
        bool got = co_await h2->server.pump([&] { return h2->server.responses.contains(1); });
        REQUIRE(got);
        h2->server.s.reset(1, http11_required);
        got = co_await h2->server.pump([] { return true; });
        REQUIRE(got);

        // the server closes the HTTP/1.1 connection without answering, or the caller stops the request first.
        auto h1 = co_await accept_h1(host.listener);
        REQUIRE(h1.has_value());
        const auto sent = co_await h1->read_request();
        REQUIRE(sent.has_value());
        if (cancel) stop.request_stop();
        else h1->socket.shutdown_write();

        got = co_await out.wait(&sched);
        REQUIRE(got);
        if (cancel) CHECK(out.err == std::errc::operation_canceled);
        else CHECK(out.err == net::io::status_condition::closed);
    };

    sched.run_to_completion(test());
}

TEST_CASE("at max_connections_per_host, a request for HTTP/1.1 closes its host's idle HTTP/2 connection",
          "[http][http2][client][http11-required][limits]")
{
    std::atomic<std::uint64_t> done{0};
    // used by the requests' tasks, which a failing REQUIRE may leave to the scheduler to end.
    outcome            out;
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};

    // a request waiting for room would wait out connect_timeout.
    auto options                     = prior_knowledge();
    options.max_connections_per_host = 1;
    options.connect_timeout          = 20s;
    http::client client{&sched, options};
    REQUIRE(sched.spawn(watch(&sched, &done, {&host.listener})));

    const auto test = [&] -> task<>
    {
        const net::util::defer finished{[&] noexcept { test::stop_probe::bump(&sched, done); }};

        REQUIRE(sched.spawn(send_with(&sched, &client, request(http::request_method::GET, host.url("/")), &out)));

        auto h2  = co_await accept_h2(&host);
        bool got = co_await h2->server.pump([&] { return h2->server.responses.contains(1); });
        REQUIRE(got);
        h2->server.s.reset(1, http11_required);
        got = co_await h2->server.pump([] { return true; });
        REQUIRE(got);

        auto h1 = co_await accept_h1(host.listener);
        REQUIRE(h1.has_value());
        auto req = co_await h1->read_request();
        REQUIRE(req.has_value());
        got = co_await h1->send("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"sv);
        REQUIRE(got);

        got = co_await out.wait(&sched);
        REQUIRE(got);
        CHECK(out.body == "ok");

        // the HTTP/2 connection was closed to make room.
        got = co_await h2->server.pump([&] { return h2->server.closed; });
        CHECK(got);
        CHECK(h2->server.goaway);
        CHECK(client.stats() == http::connection_stats{.open = 1, .idle = 1});
    };

    sched.run_to_completion(test());
}

TEST_CASE("after HTTP_1_1_REQUIRED, a redirect to the same origin goes over HTTP/1.1, and to another tries HTTP/2",
          "[http][http2][client][http11-required][redirect]")
{
    const bool same = GENERATE(true, false);
    CAPTURE(same);

    std::atomic<std::uint64_t> done{0};
    // used by the requests' tasks, which a failing REQUIRE may leave to the scheduler to end.
    outcome            out;
    net::io::scheduler sched{make_pool()};
    fake_host          host{&sched};
    fake_host          other{&sched};

    auto options          = prior_knowledge();
    options.max_redirects = 5;
    http::client client{&sched, options};
    REQUIRE(sched.spawn(watch(&sched, &done, {&host.listener, &other.listener})));

    const auto test = [&] -> task<>
    {
        const net::util::defer finished{[&] noexcept { test::stop_probe::bump(&sched, done); }};

        REQUIRE(sched.spawn(send_with(&sched, &client, request(http::request_method::GET, host.url("/start")), &out)));

        auto h2  = co_await accept_h2(&host);
        bool got = co_await h2->server.pump([&] { return h2->server.responses.contains(1); });
        REQUIRE(got);
        h2->server.s.reset(1, http11_required);
        got = co_await h2->server.pump([] { return true; });
        REQUIRE(got);

        auto h1 = co_await accept_h1(host.listener);
        REQUIRE(h1.has_value());
        auto req = co_await h1->read_request();
        REQUIRE(req.has_value());
        CHECK(req->first.starts_with("GET /start HTTP/1.1\r\n"));

        const auto location = same ? std::string{"/next"} : other.url("/there");
        got =
            co_await h1->send(std::format("HTTP/1.1 302 Found\r\nLocation: {}\r\nContent-Length: 0\r\n\r\n", location));
        REQUIRE(got);

        // NOTE: kept until the client has its response: closing a socket with unread data resets the connection
        //       (on Linux), dropping a response the client hasn't read yet.
        std::unique_ptr<played> there;
        if (same)
        {
            // on the same HTTP/1.1 connection, rather than the HTTP/2 one.
            req = co_await h1->read_request();
            REQUIRE(req.has_value());
            CHECK(req->first.starts_with("GET /next HTTP/1.1\r\n"));
            got = co_await h1->send("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nnext"sv);
            REQUIRE(got);
        }
        else
        {
            // another origin: HTTP/2, as any request to it.
            there = co_await accept_h2(&other);
            got   = co_await there->server.pump([&] { return there->server.responses.contains(1); });
            REQUIRE(got);
            CHECK(field(there->server.responses[1], ":path") == "/there");
            there->server.respond(1, "200", "there");
            got = co_await there->server.pump([] { return true; });
            REQUIRE(got);
        }

        got = co_await out.wait(&sched);
        REQUIRE(got);
        CHECK(out.err == std::error_code{});
        CHECK(out.body == (same ? "next" : "there"));

        // nothing more came on the first HTTP/2 connection.
        got = co_await h2->server.sync();
        REQUIRE(got);
        CHECK_FALSE(h2->server.responses.contains(3));
    };

    sched.run_to_completion(test());
}

TEST_CASE("an HTTP/2 request opened after GOAWAY with HTTP_1_1_REQUIRED says so, unsent",
          "[http][http2][client][http11-required]")
{
    net::io::scheduler sched{make_pool()};

    const auto test = [](net::io::scheduler* sched) -> task<>
    {
        auto listener = net::listener::make(sched, "127.0.0.1", "0").value();
        auto client   = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(listener.local_port()));
        REQUIRE(client.has_value());
        auto accepted = co_await listener.accept();
        REQUIRE(accepted.has_value());

        auto conn = std::make_shared<http::detail::http2_client_connection>(
            sched,
            std::make_unique<http::detail::connection>(std::move(*client)),
            http::detail::http2_client_options{},
            http::detail::http2_client_connection::callbacks{});
        REQUIRE(conn->start());
        test::h2_client server{&*accepted, &*accepted, &*accepted, http2::role::server};

        // a stream reserved, then the server's GOAWAY.
        REQUIRE(conn->reserve());
        server.s.goaway(0, http11_required);
        bool got = co_await server.sync();
        REQUIRE(got);
        CHECK_FALSE(conn->accepting());

        auto req = request(http::request_method::POST, "http://127.0.0.1/");
        req.set_body("unread");

        http::detail::http2_client_connection::send_state state;
        const auto                                        resp = co_await conn->send(req, &state);
        REQUIRE_FALSE(resp.has_value());
        CHECK(resp.error() == http11_required);
        CHECK(state.unsent);

        got = co_await server.pump([&] { return server.closed; }, 3s);
        CHECK(got);
    };

    sched.run_to_completion(test(&sched));
}
