// Switching an HTTP/1.1 connection to another protocol (http::detail::switch_protocols(), for WebSockets): the 101,
// the tunnel's bytes, how the switch meets the watch, and the connection's end, limits and lifecycle once it's
// switched.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
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
#include "http/h2_tunnels.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/http2.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "http/tunnel.hpp"
#include "http/tunnel_echo.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/string_writer.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "tcp.hpp"

namespace http = net::http;

namespace
{

using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

using net::coro::task;

constexpr std::string_view upgrade_request =
    "GET /echo HTTP/1.1\r\nHost: x\r\nUpgrade: echo\r\nConnection: Upgrade\r\n\r\n";

// upgrade_fields are the 101's, as a protocol's handshake would send them.
http::headers upgrade_fields()
{
    http::headers fields;
    fields.set("Upgrade"sv, "echo"sv);
    fields.set("Connection"sv, "Upgrade"sv);
    return fields;
}

// head_of is what came of a response before its body: its head, up to the empty line.
std::string_view head_of(std::string_view received)
{
    const auto end = received.find("\r\n\r\n"sv);
    return end == std::string_view::npos ? received : received.substr(0, end + 4);
}

// after_head is what came after the head.
std::string_view after_head(std::string_view received) { return received.substr(head_of(received).size()); }

// connect connects to the server on port.
task<std::expected<net::tcp_socket, std::error_code>> connect(net::io::scheduler* sched, std::uint16_t port)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    if (conn.has_value()) conn->set_read_timeout(5s);
    co_return conn;
}

// make_server makes a server of router on a port of its own, and serves it.
std::unique_ptr<http::server>
make_server(net::io::scheduler* sched, http::router router, http::server_config cfg = {.host = "127.0.0.1"})
{
    cfg.host = "127.0.0.1";
    cfg.port = "0";

    auto server = std::make_unique<http::server>(sched, std::move(router), cfg);
    REQUIRE(sched->spawn(server->serve()));
    return server;
}

// count counts the occurrences of what in str.
std::size_t count(std::string_view str, std::string_view what)
{
    std::size_t n = 0;
    for (auto pos = str.find(what); pos != std::string_view::npos; pos = str.find(what, pos + what.size())) ++n;
    return n;
}

// failure_text names a failed switch's error, or says it switched.
std::string failure_text(const std::expected<http::detail::tunnel*, std::error_code>& switched)
{
    if (switched.has_value()) return "switched";
    if (switched.error() == std::errc::invalid_argument) return "invalid_argument";
    if (switched.error() == std::errc::not_supported) return "not_supported";
    return switched.error().message();
}

// respond responds with 200 and text.
task<void> respond(http::response_writer& rw, std::string text)
{
    auto* body = co_await rw.send(http::status::OK, text.size());
    co_await net::io::write_all(*body, text);
}

}

TEST_CASE("an HTTP/1.1 handler switches protocols, and has the connection's bytes until it returns",
          "[http][server][tunnel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // whether the handler waits before it switches, so the connection's watched meanwhile (unless the client sent
    // bytes along with its request, which are buffered: then there's no need), and the switch ends the watch.
    const bool waits = GENERATE(false, true);

    // whether the client sends the new protocol's bytes along with its request, which the switch keeps.
    const bool early = GENERATE(false, true);
    CAPTURE(waits, early);

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   if (waits) (void)co_await sched.sleep_until(net::io::clock::now() + 50ms);

                   // the response's own fields go with the 101.
                   rw.headers().set("Set-Cookie"sv, "a=b"sv);
                   rw.headers().set("Content-Length"sv, "5"sv);

                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value()) co_return;
                   auto& t = **switched;

                   // nothing more can be sent as a response.
                   bool threw = false;
                   try
                   {
                       (void)co_await rw.send(http::status::OK, 0);
                   }
                   catch (const std::logic_error&)
                   {
                       threw = true;
                   }

                   const auto echoed = co_await test::echo(t);

                   // left written, which the connection's end sends.
                   (void)co_await net::io::write_all(t.writer(),
                                                     std::format("switched={} sent={} status={} threw={} echo={}",
                                                                 rw.switched(),
                                                                 rw.sent(),
                                                                 static_cast<int>(rw.status_code()),
                                                                 threw,
                                                                 (echoed ? echoed.message() : "ok")));
               });

    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::pair<std::string, std::error_code>>
    {
        std::string received;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{received, conn.error()};

        (void)co_await net::io::write_all(*conn, std::string{upgrade_request} + (early ? "early\n" : ""));
        if (auto err = co_await test::read_until(*conn, received, "\r\n\r\n"sv)) co_return std::pair{received, err};
        if (early)
        {
            if (auto err = co_await test::read_until(*conn, received, "early\n"sv)) co_return std::pair{received, err};
        }

        (void)co_await net::io::write_all(*conn, "ping\n"sv);
        if (auto err = co_await test::read_until(*conn, received, "ping\n"sv)) co_return std::pair{received, err};

        (void)co_await net::io::write_all(*conn, "bye\n"sv);
        auto err = co_await test::read_to_end(*conn, received);
        co_return std::pair{received, err};
    };

    auto [received, err] = sched.run_to_completion(client());
    CAPTURE(received);

    const auto head = head_of(received);
    CHECK(head.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(head.contains("\r\nUpgrade: echo\r\n"));
    CHECK(head.contains("\r\nConnection: Upgrade\r\n"));
    CHECK(head.contains("\r\nSet-Cookie: a=b\r\n"));
    CHECK(head.contains("\r\nDate: "));
    CHECK_FALSE(head.contains("Content-Length"));
    CHECK_FALSE(head.contains("Transfer-Encoding"));

    // the server ends the connection once the handler returns, having sent what it left written.
    CHECK(after_head(received)
          == std::string{early ? "early\n" : ""} + "ping\nbye\nswitched=true sent=true status=101 threw=true echo=ok");
    CHECK(err == net::io::errc::end_of_stream);
}

TEST_CASE("an HTTP/1.1 handler switches protocols once its watch has seen the client stop sending",
          "[http][server][tunnel][watch]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   // the watch's read finds the end of the stream, which stops the token.
                   const auto waited = co_await sched.sleep_until(net::io::clock::now() + 5s, req.stop);

                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value()) co_return;
                   auto& t = **switched;

                   std::array<std::byte, 16> buf{};
                   const auto                read = co_await t.reader().read(buf);
                   (void)co_await net::io::write_all(
                       t.writer(),
                       std::format("stopped={} read={} {}",
                                   waited == std::errc::operation_canceled,
                                   read.count,
                                   read.err == net::io::errc::end_of_stream ? "end" : read.err.message()));
               });

    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::pair<std::string, std::error_code>>
    {
        std::string received;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{received, conn.error()};

        (void)co_await net::io::write_all(*conn, upgrade_request);
        conn->shutdown_write();

        auto err = co_await test::read_to_end(*conn, received);
        co_return std::pair{received, err};
    };

    auto [received, err] = sched.run_to_completion(client());
    CAPTURE(received);
    CHECK(head_of(received).starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(after_head(received) == "stopped=true read=0 end");
    CHECK(err == net::io::errc::end_of_stream);
}

TEST_CASE("an HTTP/1.1 request can't switch protocols until its body has been read to its end",
          "[http][server][tunnel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.POST("/echo",
                [&](http::server_request& req, http::response_writer& rw) -> task<void>
                {
                    const auto before = co_await http::detail::switch_protocols(rw, upgrade_fields());
                    auto       body   = co_await net::io::read_all(*req.body);

                    auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                    if (!switched.has_value()) co_return;
                    auto& t = **switched;

                    (void)co_await net::io::write_all(
                        t.writer(),
                        std::format("before={} body={}", failure_text(before), body.value_or("failed")));
                    (void)co_await test::echo(t);
                });

    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::pair<std::string, std::error_code>>
    {
        std::string received;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{received, conn.error()};

        (void)co_await net::io::write_all(
            *conn,
            "POST /echo HTTP/1.1\r\nHost: x\r\nUpgrade: echo\r\nConnection: Upgrade\r\nContent-Length: 5\r\n\r\nhello"
            "bye\n"sv);

        auto err = co_await test::read_to_end(*conn, received);
        co_return std::pair{received, err};
    };

    auto [received, err] = sched.run_to_completion(client());
    CAPTURE(received);

    // nothing was sent for the first try; the body wasn't taken for the tunnel's, nor what followed it for the body.
    CHECK(head_of(received).starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(after_head(received) == "before=invalid_argument body=hellobye\n");
    CHECK(err == net::io::errc::end_of_stream);
}

TEST_CASE("an HTTP/1.1 handler whose switch fails can still respond", "[http][server][tunnel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    struct test_case
    {
        std::string_view name;
        std::string_view version;   // the request's
        std::string_view field;     // set in the 101's fields, with a value that can't be sent, if not empty
        std::string_view own_field; // and in the response's own
        std::string_view expected;
    };

    const auto tc = GENERATE(test_case{.name      = "a field with a line break",
                                       .version   = "1.1",
                                       .field     = "X-Bad",
                                       .own_field = "",
                                       .expected  = "invalid_argument"},
                             test_case{.name      = "a field of the response's with a line break",
                                       .version   = "1.1",
                                       .field     = "",
                                       .own_field = "X-Bad",
                                       .expected  = "invalid_argument"},
                             test_case{.name      = "an HTTP/1.0 request (whose Upgrade a server must ignore)",
                                       .version   = "1.0",
                                       .field     = "",
                                       .own_field = "",
                                       .expected  = "not_supported"});
    CAPTURE(tc.name);

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto fields = upgrade_fields();
                   if (!tc.field.empty()) fields.set(tc.field, "a\r\nb"sv);
                   if (!tc.own_field.empty()) rw.headers().set(tc.own_field, "a\r\nb"sv);

                   const auto switched = co_await http::detail::switch_protocols(rw, fields);
                   const auto text =
                       std::format("{} switched={} sent={}", failure_text(switched), rw.switched(), rw.sent());

                   if (!tc.own_field.empty()) rw.headers().remove(tc.own_field);
                   co_await respond(rw, text);
               });

    auto server = make_server(&sched, std::move(router));

    // and the connection goes on: the next request is served.
    const auto client = [&] -> task<std::pair<std::string, std::error_code>>
    {
        std::string received;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{received, conn.error()};

        const auto request = [&](std::string_view connection)
        {
            return std::format("GET /echo HTTP/{}\r\nHost: x\r\nUpgrade: echo\r\nConnection: Upgrade, {}\r\n\r\n",
                               tc.version,
                               connection);
        };
        (void)co_await net::io::write_all(*conn, request("keep-alive") + request("close"));

        auto err = co_await test::read_to_end(*conn, received);
        co_return std::pair{received, err};
    };

    auto [received, err] = sched.run_to_completion(client());
    CAPTURE(received);
    CHECK_FALSE(received.contains(" 101 "));
    CHECK(count(received, std::format("HTTP/{} 200 OK\r\n", tc.version)) == 2);
    CHECK(count(received, std::format("\r\n\r\n{} switched=false sent=false", tc.expected)) == 2);
    CHECK(err == net::io::errc::end_of_stream);
}

TEST_CASE("an HTTP/1.1 handler can't switch protocols once it has responded", "[http][server][tunnel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto* body = co_await rw.send(http::status::OK);

                   // NOTE: a co_await can't be in a catch block.
                   std::string outcome;
                   try
                   {
                       (void)co_await http::detail::switch_protocols(rw, upgrade_fields());
                       outcome = "switched";
                   }
                   catch (const std::logic_error&)
                   {
                       outcome = "refused";
                   }
                   co_await net::io::write_all(*body, outcome);
               });

    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::pair<std::string, std::error_code>>
    {
        std::string received;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{received, conn.error()};

        (void)co_await net::io::write_all(
            *conn,
            "GET /echo HTTP/1.1\r\nHost: x\r\nUpgrade: echo\r\nConnection: Upgrade, close\r\n\r\n"sv);
        auto err = co_await test::read_to_end(*conn, received);
        co_return std::pair{received, err};
    };

    auto [received, err] = sched.run_to_completion(client());
    CAPTURE(received);
    CHECK(received.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(received.ends_with("\r\n\r\n7\r\nrefused\r\n0\r\n\r\n"));
}

TEST_CASE("an HTTP/2 handler can't switch protocols with a 101", "[http][server][tunnel][http2]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   const auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   co_await respond(rw, failure_text(switched));
               });

    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::string>
    {
        http::client client{&sched, {.http2_prior_knowledge = true}};

        auto resp = co_await client.get(std::format("http://127.0.0.1:{}/echo", server->local_port()));
        if (!resp.has_value()) co_return "error: " + resp.error().message();
        if (resp->version.major != 2) co_return "not HTTP/2";

        auto body = co_await net::io::read_all(*resp->body);
        co_return body.has_value() ? *body : "error: " + body.error().message();
    };

    CHECK(sched.run_to_completion(client()) == "not_supported");
}

TEST_CASE("aborting the server ends a switched connection's reads, and stops its handler's token",
          "[http][server][tunnel][cancel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value()) co_return;
                   auto& t = **switched;

                   const auto echoed = co_await test::echo(t);
                   (void)co_await net::io::write_all(t.writer(),
                                                     std::format("echo={} stopped={}",
                                                                 echoed == net::io::status_condition::closed,
                                                                 req.stop.stop_requested()));
               });

    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::pair<std::string, std::error_code>>
    {
        std::string received;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{received, conn.error()};

        (void)co_await net::io::write_all(*conn, upgrade_request);
        if (auto err = co_await test::read_until(*conn, received, "\r\n\r\n"sv)) co_return std::pair{received, err};

        server->abort();
        auto err = co_await test::read_to_end(*conn, received);

        // and join() returns, as the connection's ended.
        co_await server->join();
        co_return std::pair{received, err};
    };

    auto [received, err] = sched.run_to_completion(client());
    CAPTURE(received);
    CHECK(head_of(received).starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(after_head(received) == "echo=true stopped=true");
    CHECK(err == net::io::errc::end_of_stream);
}

TEST_CASE("closing the server lets a switched connection's handler finish, telling it through the tunnel",
          "[http][server][tunnel][close]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    std::atomic<std::uint64_t> joined  = 0;
    std::atomic<std::uint64_t> checked = 0; // the handler has looked at closing() before the server's close()

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value()) co_return;
                   auto& t = **switched;

                   const bool closing_before = t.closing().stop_requested();
                   checked.store(1);
                   sched.notify_all(checked);

                   const auto echoed = co_await test::echo(t);
                   (void)co_await net::io::write_all(t.writer(),
                                                     std::format("closing={},{} echo={}",
                                                                 closing_before,
                                                                 t.closing().stop_requested(),
                                                                 (echoed ? echoed.message() : "ok")));
               });

    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::pair<std::string, std::error_code>>
    {
        std::string received;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{received, conn.error()};

        (void)co_await net::io::write_all(*conn, upgrade_request);
        if (auto err = co_await test::read_until(*conn, received, "\r\n\r\n"sv)) co_return std::pair{received, err};

        // the 101 arrives once it's flushed, before the handler has looked: it's closed after.
        const auto looked = net::io::clock::now() + 5s;
        while (checked.load() == 0)
        {
            if (co_await sched.wait_on(checked, 0, looked)) break;
        }

        server->close();
        REQUIRE(sched.spawn(
            [](http::server* server, net::io::scheduler* sched, std::atomic<std::uint64_t>* joined) -> task<void>
            {
                co_await server->join();
                joined->store(1);
                sched->notify_all(*joined);
            }(server.get(), &sched, &joined)));

        // the tunnel goes on: the server's close() leaves it to its handler.
        (void)co_await net::io::write_all(*conn, "ping\n"sv);
        if (auto err = co_await test::read_until(*conn, received, "ping\n"sv)) co_return std::pair{received, err};
        const auto joined_early = joined.load();

        (void)co_await net::io::write_all(*conn, "bye\n"sv);
        auto err = co_await test::read_to_end(*conn, received);

        const auto deadline = net::io::clock::now() + 5s;
        while (joined.load() == 0)
        {
            if (co_await sched.wait_on(joined, 0, deadline)) break;
        }

        received += std::format(" joined={},{}", joined_early, joined.load());
        co_return std::pair{received, err};
    };

    auto [received, err] = sched.run_to_completion(client());
    CAPTURE(received);
    CHECK(head_of(received).starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(after_head(received) == "ping\nbye\nclosing=false,true echo=ok joined=0,1");
    CHECK(err == net::io::errc::end_of_stream);
}

TEST_CASE("a switched connection counts against max_connections until its handler returns",
          "[http][server][tunnel][limits]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (switched.has_value()) (void)co_await test::echo(**switched);
               });
    router.GET("/plain", [](http::server_request& /*req*/, http::response_writer& rw) { return respond(rw, "plain"); });

    auto server = make_server(&sched, std::move(router), {.max_connections = 1});

    const auto client = [&] -> task<std::string>
    {
        std::string switched_received;
        auto        first = co_await connect(&sched, server->local_port());
        if (!first.has_value()) co_return "connect failed";

        (void)co_await net::io::write_all(*first, upgrade_request);
        if (co_await test::read_until(*first, switched_received, "\r\n\r\n"sv)) co_return "no 101";

        // the second connects (the kernel accepts it), but isn't served while the first is open.
        std::string received;
        auto        second = co_await connect(&sched, server->local_port());
        if (!second.has_value()) co_return "connect failed";

        (void)co_await net::io::write_all(*second, "GET /plain HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"sv);
        second->set_read_deadline(net::io::clock::now() + 300ms);
        const auto waited = co_await test::read_until(*second, received, "plain"sv);
        if (waited != std::errc::timed_out) co_return "served while the first was open: " + waited.message();
        second->set_read_deadline(std::nullopt);

        (void)co_await net::io::write_all(*first, "bye\n"sv);
        (void)co_await test::read_to_end(*first, switched_received);

        (void)co_await test::read_to_end(*second, received);
        co_return received;
    };

    const auto received = sched.run_to_completion(client());
    CAPTURE(received);
    CHECK(received.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(received.ends_with("\r\n\r\nplain"));
}

TEST_CASE("aborting a tunnel ends its reads, and the connection without what wasn't flushed",
          "[http][server][tunnel][cancel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value()) co_return;
                   auto& t = **switched;

                   (void)co_await net::io::write_all(t.writer(), "flushed "sv);
                   (void)co_await t.writer().flush();

                   (void)co_await net::io::write_all(t.writer(), "lost"sv);
                   t.abort();

                   // the client's sent nothing more, nor ended what it sends.
                   std::array<std::byte, 16> buf{};
                   const auto                read = co_await t.reader().read(buf);
                   if (read.err == net::io::status_condition::closed) co_return;

                   (void)co_await net::io::write_all(t.writer(), "read"sv);
                   (void)co_await t.writer().flush();
               });

    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::pair<std::string, std::error_code>>
    {
        std::string received;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{received, conn.error()};

        (void)co_await net::io::write_all(*conn, upgrade_request);
        auto err = co_await test::read_to_end(*conn, received);
        co_return std::pair{received, err};
    };

    auto [received, err] = sched.run_to_completion(client());
    CAPTURE(received);
    CHECK(head_of(received).starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(after_head(received) == "flushed ");
    CHECK(err == net::io::errc::end_of_stream);
}

TEST_CASE("a switched connection's reads have only the tunnel's timeout", "[http][server][tunnel][timeout]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // whether the handler waits before it switches, so the switch ends a watch (whose read has a deadline of now).
    const bool waits = GENERATE(false, true);
    CAPTURE(waits);

    http::router router;
    router.GET(
        "/echo",
        [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
        {
            if (waits) (void)co_await sched.sleep_until(net::io::clock::now() + 50ms);

            auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
            if (!switched.has_value()) co_return;
            auto& t = **switched;

            // longer than the request body's timeout, which the switch took off.
            const auto first = co_await t.reader().fill();
            const auto data  = t.reader().buffered();
            const auto got   = std::string_view{reinterpret_cast<const char*>(data.data()), data.size()};
            (void)co_await net::io::write_all(t.writer(),
                                              std::format("got={} ({}) ", got, (first ? first.message() : "ok")));
            t.reader().consume(data.size());
            (void)co_await t.writer().flush();

            t.set_read_timeout(100ms);
            const auto second = co_await t.reader().fill();
            (void)co_await net::io::write_all(t.writer(),
                                              second == std::errc::timed_out ? "timed out"sv : "not timed out"sv);
        });

    auto server = make_server(&sched, std::move(router), {.body_read_timeout = 1s});

    const auto client = [&] -> task<std::pair<std::string, std::error_code>>
    {
        std::string received;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{received, conn.error()};

        (void)co_await net::io::write_all(*conn, upgrade_request);
        if (auto err = co_await test::read_until(*conn, received, "\r\n\r\n"sv)) co_return std::pair{received, err};

        (void)co_await sched.sleep_until(net::io::clock::now() + 1500ms);
        (void)co_await net::io::write_all(*conn, "late"sv);

        auto err = co_await test::read_to_end(*conn, received);
        co_return std::pair{received, err};
    };

    auto [received, err] = sched.run_to_completion(client());
    CAPTURE(received);
    CHECK(after_head(received) == "got=late (ok) timed out");
    CHECK(err == net::io::errc::end_of_stream);
}

TEST_CASE("a switched connection's writes keep the server's write_timeout", "[http][server][tunnel][timeout]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    std::atomic<std::uint64_t> done      = 0;
    std::atomic<bool>          timed_out = false;

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (switched.has_value())
                   {
                       // until the client, which doesn't read, has as much as the connection holds.
                       const std::string chunk(std::size_t{64} * 1'024, 'x');
                       while (true)
                       {
                           auto wrote = co_await net::io::write_all((*switched)->writer(), chunk);
                           if (!wrote.err) wrote = co_await (*switched)->writer().flush();
                           if (wrote.err)
                           {
                               timed_out.store(wrote.err == std::errc::timed_out);
                               break;
                           }
                       }
                   }

                   done.store(1);
                   sched.notify_all(done);
               });

    auto server = make_server(&sched, std::move(router), {.write_timeout = 1s});

    const auto client = [&] -> task<bool>
    {
        std::string received;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return false;

        (void)co_await net::io::write_all(*conn, upgrade_request);
        if (co_await test::read_until(*conn, received, "\r\n\r\n"sv)) co_return false;

        const auto deadline = net::io::clock::now() + 10s;
        while (done.load() == 0)
        {
            if (co_await sched.wait_on(done, 0, deadline)) co_return false;
        }
        co_return true;
    };

    CHECK(sched.run_to_completion(client()));
    CHECK(timed_out.load());
}

TEST_CASE("a tunnel's stop token cancels a waiting read, which reads nothing", "[http][server][tunnel][cancel]")
{
    // NOTE: before the scheduler, as the handler uses it.
    std::stop_source stop;

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value()) co_return;
                   auto& t = **switched;

                   t.set_stop_token(stop.get_token());
                   (void)co_await net::io::write_all(t.writer(), "ready\n"sv);
                   (void)co_await t.writer().flush();

                   const auto canceled = co_await t.reader().fill();
                   (void)co_await net::io::write_all(
                       t.writer(),
                       std::format("{} {}\n",
                                   canceled == std::errc::operation_canceled ? "canceled" : canceled.message(),
                                   t.reader().size()));
                   (void)co_await t.writer().flush();

                   t.set_stop_token({});
                   (void)co_await test::echo(t);
               });

    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::pair<std::string, std::error_code>>
    {
        std::string received;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{received, conn.error()};

        (void)co_await net::io::write_all(*conn, upgrade_request);
        if (auto err = co_await test::read_until(*conn, received, "ready\n"sv)) co_return std::pair{received, err};

        stop.request_stop();
        if (auto err = co_await test::read_until(*conn, received, "0\n"sv)) co_return std::pair{received, err};

        (void)co_await net::io::write_all(*conn, "after\nbye\n"sv);
        auto err = co_await test::read_to_end(*conn, received);
        co_return std::pair{received, err};
    };

    auto [received, err] = sched.run_to_completion(client());
    CAPTURE(received);
    CHECK(after_head(received) == "ready\ncanceled 0\nafter\nbye\n");
    CHECK(err == net::io::errc::end_of_stream);
}

TEST_CASE("a response_writer without a connection to switch can't switch protocols", "[http][response][tunnel]")
{
    // e.g. what the server's own error responses are written with.
    net::io::string_writer<char> out;
    http::server_response        resp{
        .version = {.major = 1, .minor = 1}
    };
    http::response_writer rw{&out, &resp, http::http11::response_encode};

    auto switching = http::detail::switch_protocols(rw, upgrade_fields());
    REQUIRE(switching.finished_without_coroutine());

    const auto switched = std::move(switching).result();
    CHECK(failure_text(switched) == "not_supported");
    CHECK_FALSE(rw.sent());
    CHECK(out.build().empty());
}

// ---- HTTP/2: an extended CONNECT's stream as a tunnel (RFC 8441)

namespace
{

using test::h2_tunnels;

// connect_h2 connects to the server on port, for HTTP/2 with prior knowledge (h2_tunnels limits its own waits).
task<std::expected<net::tcp_socket, std::error_code>> connect_h2(net::io::scheduler* sched, std::uint16_t port)
{
    return net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
}

// tunnel_result is what a client of a tunnel over HTTP/2 saw: the response's head, what it read, and how that ended.
struct tunnel_result
{
    std::string                                      status;
    std::vector<std::pair<std::string, std::string>> headers;
    bool                                             head_ended = false;
    std::string                                      received;
    std::error_code                                  err;
    std::vector<std::pair<std::string, std::string>> trailers;
};

// over_h2 connects, opens a stream with an extended CONNECT for path (:protocol websocket, the one the server takes),
// waits for its head, and runs steps over the stream (as a reader and writer) with the result to fill.
template<typename Steps>
task<tunnel_result> over_h2(net::io::scheduler* sched, std::uint16_t port, std::string path, Steps steps)
{
    tunnel_result result;
    auto          conn = co_await connect_h2(sched, port);
    if (!conn.has_value())
    {
        result.err = conn.error();
        co_return result;
    }

    co_await test::with_tunnels(sched,
                                &*conn,
                                &*conn,
                                &*conn,
                                [&](h2_tunnels& t) -> task<>
                                {
                                    auto id = co_await t.open(path, "websocket");
                                    if (!id.has_value())
                                    {
                                        result.err = id.error();
                                        co_return;
                                    }

                                    const auto head = co_await t.response(*id);
                                    if (!head.has_value()) co_return;
                                    result.status     = head->status;
                                    result.headers    = head->headers;
                                    result.head_ended = head->head_ended;

                                    h2_tunnels::stream s{&t, *id};
                                    co_await steps(t, *id, s, result);

                                    if (const auto done = co_await t.done(*id)) result.trailers = done->trailers;
                                });
    co_return result;
}

// has_field is whether a response's head has a field of that name.
bool has_field(const tunnel_result& r, std::string_view name)
{
    return std::ranges::any_of(r.headers, [&](const auto& f) { return f.first == name; });
}

}

TEST_CASE("an HTTP/2 handler opens an extended CONNECT's stream as a tunnel, which ends with the handler",
          "[http][server][tunnel][http2]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   rw.headers().set("X-Kept"sv, "yes"sv);
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value())
                   {
                       co_await respond(rw, failure_text(switched));
                       co_return;
                   }
                   auto& t = **switched;

                   const auto echoed = co_await test::echo(t);
                   (void)co_await net::io::write_all(t.writer(),
                                                     std::format("switched={} sent={} protocol={} echo={}",
                                                                 rw.switched(),
                                                                 rw.sent(),
                                                                 req.protocol,
                                                                 (echoed ? echoed.message() : "ok")));
                   (void)co_await t.writer().flush();

                   // trailers have no place on a tunnel, whose stream ends with DATA: they're dropped.
                   rw.trailers().set("X-Trailer"sv, "dropped"sv);
               });

    auto server = make_server(&sched, std::move(router));

    const auto result = sched.run_to_completion(over_h2(
        &sched,
        server->local_port(),
        "/echo",
        [](h2_tunnels& /*t*/, net::http::http2::stream_id /*id*/, h2_tunnels::stream& s, tunnel_result& r) -> task<>
        {
            (void)co_await net::io::write_all(s, "hello\n"sv);
            if (co_await test::read_until(s, r.received, "hello\n"sv)) co_return;
            (void)co_await net::io::write_all(s, "bye\n"sv);
            r.err = co_await test::read_to_end(s, r.received);
        }));

    CAPTURE(result.received);
    CHECK(result.status == "200");
    CHECK_FALSE(result.head_ended);
    CHECK(has_field(result, "x-kept"));
    CHECK_FALSE(has_field(result, "upgrade"));
    CHECK_FALSE(has_field(result, "connection"));
    CHECK_FALSE(has_field(result, "content-length"));
    CHECK(result.received == "hello\nbye\nswitched=true sent=true protocol=websocket echo=ok");
    CHECK(result.err == net::io::errc::end_of_stream);
    CHECK(result.trailers.empty());
}

TEST_CASE("an HTTP/2 tunnel's reads have only the tunnel's timeout, which leaves the stream going",
          "[http][server][tunnel][http2][timeout]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value()) co_return;
                   auto& t = **switched;

                   // longer than the request body's timeout, which would have reset the stream.
                   const auto first = co_await t.reader().fill();
                   const auto data  = t.reader().buffered();
                   (void)co_await net::io::write_all(
                       t.writer(),
                       std::format("got={} ({}) ",
                                   std::string_view{reinterpret_cast<const char*>(data.data()), data.size()},
                                   (first ? first.message() : "ok")));
                   t.reader().consume(data.size());
                   (void)co_await t.writer().flush();

                   // the tunnel's own: the read fails, having read nothing, and the stream goes on.
                   t.set_read_timeout(100ms);
                   const auto start  = net::io::clock::now();
                   const auto second = co_await t.reader().fill();
                   const bool waited = net::io::clock::now() - start >= 100ms;
                   (void)co_await net::io::write_all(
                       t.writer(),
                       std::format("{} {} {}\n",
                                   second == std::errc::timed_out ? "timed out"sv : "not timed out"sv,
                                   waited,
                                   t.reader().size()));
                   (void)co_await t.writer().flush();

                   t.set_read_timeout(std::nullopt);
                   (void)co_await test::echo(t);
               });

    auto server = make_server(&sched, std::move(router), {.host = "127.0.0.1", .body_read_timeout = 1s});

    const auto result = sched.run_to_completion(over_h2(
        &sched,
        server->local_port(),
        "/echo",
        [&](h2_tunnels& /*t*/, net::http::http2::stream_id /*id*/, h2_tunnels::stream& s, tunnel_result& r) -> task<>
        {
            (void)co_await sched.sleep_until(net::io::clock::now() + 1500ms);
            (void)co_await net::io::write_all(s, "late"sv);
            if (co_await test::read_until(s, r.received, "\n"sv)) co_return;

            (void)co_await net::io::write_all(s, "after\nbye\n"sv);
            r.err = co_await test::read_to_end(s, r.received);
        }));

    CAPTURE(result.received);
    CHECK(result.received == "got=late (ok) timed out true 0\nafter\nbye\n");
    CHECK(result.err == net::io::errc::end_of_stream);
}

TEST_CASE("an HTTP/2 tunnel's read deadline, set from another task, ends a waiting read, which reads nothing",
          "[http][server][tunnel][http2][timeout]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value()) co_return;
                   auto& t = **switched;

                   // a deadline of now, set while the read waits, as the server's close() has a WebSocket do.
                   REQUIRE(sched.spawn(
                       [](http::detail::tunnel* t, net::io::scheduler* sched) -> task<void>
                       {
                           (void)co_await sched->sleep_until(net::io::clock::now() + 100ms);
                           t->set_read_deadline(net::io::clock::now());
                       }(&t, &sched)));

                   const auto interrupted = co_await t.reader().fill();
                   (void)co_await net::io::write_all(
                       t.writer(),
                       std::format("{} {}\n",
                                   interrupted == std::errc::timed_out ? "timed out" : interrupted.message(),
                                   t.reader().size()));
                   (void)co_await t.writer().flush();

                   t.set_read_deadline(std::nullopt);
                   (void)co_await test::echo(t);
               });

    auto server = make_server(&sched, std::move(router));

    const auto result = sched.run_to_completion(over_h2(
        &sched,
        server->local_port(),
        "/echo",
        [](h2_tunnels& /*t*/, net::http::http2::stream_id /*id*/, h2_tunnels::stream& s, tunnel_result& r) -> task<>
        {
            if (co_await test::read_until(s, r.received, "\n"sv)) co_return;
            (void)co_await net::io::write_all(s, "after\nbye\n"sv);
            r.err = co_await test::read_to_end(s, r.received);
        }));

    CAPTURE(result.received);
    CHECK(result.received == "timed out 0\nafter\nbye\n");
    CHECK(result.err == net::io::errc::end_of_stream);
}

TEST_CASE(
    "an HTTP/2 tunnel's stop token cancels a waiting read, which reads nothing, and a write waiting for the window",
    "[http][server][tunnel][http2][cancel]")
{
    // NOTE: before the scheduler, as the handler uses them.
    std::stop_source           read_stop;
    std::stop_source           write_stop;
    std::atomic<std::uint64_t> written = 0;

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value()) co_return;
                   auto& t = **switched;

                   t.set_stop_token(read_stop.get_token());
                   (void)co_await net::io::write_all(t.writer(), "ready\n"sv);
                   (void)co_await t.writer().flush();

                   const auto canceled = co_await t.reader().fill();
                   (void)co_await net::io::write_all(
                       t.writer(),
                       std::format("{} {}\n",
                                   canceled == std::errc::operation_canceled ? "canceled" : canceled.message(),
                                   t.reader().size()));
                   (void)co_await t.writer().flush();

                   // a write larger than the stream's window, which the client doesn't give back until it's told.
                   t.set_stop_token(write_stop.get_token());
                   const std::string large(std::size_t{256} * 1'024, 'x');
                   const auto        wrote = co_await net::io::write_all(t.writer(), large);

                   // and one that would wait, once stopped, fails at once.
                   const auto again = co_await net::io::write_all(t.writer(), large);
                   written.store(1);
                   sched.notify_all(written);

                   t.set_stop_token({});
                   const auto name = [](std::error_code err)
                   { return err == std::errc::operation_canceled ? "canceled"s : err.message(); };
                   (void)co_await net::io::write_all(t.writer(),
                                                     std::format("\nwrite {} {}\n", name(wrote.err), name(again.err)));
                   (void)co_await t.writer().flush();
                   (void)co_await test::echo(t);
               });

    auto server = make_server(&sched, std::move(router));

    const auto result = sched.run_to_completion(over_h2(
        &sched,
        server->local_port(),
        "/echo",
        [&](h2_tunnels& /*t*/, net::http::http2::stream_id /*id*/, h2_tunnels::stream& s, tunnel_result& r) -> task<>
        {
            if (co_await test::read_until(s, r.received, "ready\n"sv)) co_return;
            read_stop.request_stop();
            if (co_await test::read_until(s, r.received, "0\n"sv)) co_return;

            // the write waits for the window (only what fits has come): then it's stopped, and returns without the
            // client reading anything more; then the rest's read.
            (void)co_await sched.sleep_until(net::io::clock::now() + 200ms);
            write_stop.request_stop();
            const auto deadline = net::io::clock::now() + 10s;
            while (written.load() == 0)
            {
                if (co_await sched.wait_on(written, 0, deadline)) co_return;
            }
            if (co_await test::read_until(s, r.received, "write "sv)) co_return;
            if (co_await test::read_until(s, r.received, "\n"sv)) co_return;

            (void)co_await net::io::write_all(s, "after\nbye\n"sv);
            r.err = co_await test::read_to_end(s, r.received);
        }));

    const auto first_line = result.received.substr(0, result.received.find('\n') + 1);
    const auto rest       = result.received.substr(result.received.find("canceled 0\n"sv) + 11);
    CHECK(first_line == "ready\n");
    CHECK(result.received.contains("ready\ncanceled 0\n"sv));

    // part of the large write went (as much as the windows allowed), not all of it.
    const auto xs = rest.find_first_not_of('x');
    CHECK(xs > 0);
    CHECK(xs < std::size_t{256} * 1'024);
    CHECK(rest.substr(xs) == "\nwrite canceled canceled\nafter\nbye\n");
    CHECK(result.err == net::io::errc::end_of_stream);
}

TEST_CASE("aborting an HTTP/2 tunnel resets its stream, without what wasn't flushed", "[http][server][tunnel][http2]")
{
    // NOTE: before the scheduler, as the handler uses them.
    std::atomic<std::uint64_t> done = 0;
    std::string                outcome;

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/echo",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value()) co_return;
                   auto& t = **switched;

                   (void)co_await net::io::write_all(t.writer(), "flushed "sv);
                   (void)co_await t.writer().flush();

                   (void)co_await net::io::write_all(t.writer(), "lost"sv);
                   t.abort();

                   std::array<std::byte, 16> buf{};
                   const auto                read = co_await t.reader().read(buf);
                   outcome                        = std::format("read: {} closed={} stopped={}",
                                                                read.err.message(),
                                                                read.err == net::io::status_condition::closed,
                                                                req.stop.stop_requested());
                   done.store(1);
                   sched.notify_all(done);
               });

    auto server = make_server(&sched, std::move(router));

    const auto result = sched.run_to_completion(over_h2(
        &sched,
        server->local_port(),
        "/echo",
        [&](h2_tunnels& /*t*/, net::http::http2::stream_id /*id*/, h2_tunnels::stream& s, tunnel_result& r) -> task<>
        {
            r.err = co_await test::read_to_end(s, r.received);

            const auto deadline = net::io::clock::now() + 10s;
            while (done.load() == 0)
            {
                if (co_await sched.wait_on(done, 0, deadline)) co_return;
            }
        }));

    CHECK(result.received == "flushed ");
    CHECK(result.err == net::http::http2::error_code::cancel);
    CHECK(outcome
          == std::format("read: {} closed=true stopped=false",
                         std::make_error_code(std::errc::connection_aborted).message()));
}

TEST_CASE("an HTTP/2 tunnel's writes keep the server's write_timeout, which resets the stream",
          "[http][server][tunnel][http2][timeout]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    std::atomic<std::uint64_t> done      = 0;
    std::atomic<bool>          timed_out = false;

    http::router router;
    router.GET("/echo",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (switched.has_value())
                   {
                       // more than the stream's window, which the client, not reading, never gives back.
                       const std::string chunk(std::size_t{64} * 1'024, 'x');
                       while (true)
                       {
                           auto wrote = co_await net::io::write_all((*switched)->writer(), chunk);
                           if (!wrote.err) wrote = co_await (*switched)->writer().flush();
                           if (wrote.err)
                           {
                               timed_out.store(wrote.err == std::errc::timed_out);
                               break;
                           }
                       }
                   }

                   done.store(1);
                   sched.notify_all(done);
               });

    auto server = make_server(&sched, std::move(router), {.host = "127.0.0.1", .write_timeout = 1s});

    const auto result = sched.run_to_completion(over_h2(
        &sched,
        server->local_port(),
        "/echo",
        [&](h2_tunnels& t, net::http::http2::stream_id id, h2_tunnels::stream& /*s*/, tunnel_result& r) -> task<>
        {
            const auto deadline = net::io::clock::now() + 10s;
            while (done.load() == 0)
            {
                if (co_await sched.wait_on(done, 0, deadline)) co_return;
            }

            const auto ended = co_await t.done(id);
            if (ended.has_value()) r.err = ended->reset;
        }));

    CHECK(timed_out.load());
    CHECK(result.err == net::http::http2::error_code::cancel);
}

namespace
{

// pattern is size bytes that differ from one to the next, so a piece in the wrong place shows.
std::string pattern(std::size_t size)
{
    std::string out(size, '\0');
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<char>('a' + (i % 23));
    return out;
}

// wait_for_word waits for word to be at least value, for up to 10 s, returning whether it was.
task<bool> wait_for_word(net::io::scheduler* sched, const std::atomic<std::uint64_t>& word, std::uint64_t value = 1)
{
    const auto deadline = net::io::clock::now() + 10s;
    while (true)
    {
        const auto seen = word.load();
        if (seen >= value) co_return true;
        if (co_await sched->wait_on(word, seen, deadline)) co_return false;
    }
}

}

TEST_CASE("an HTTP/2 tunnel's stream holds 256 KiB its handler hasn't read, and its large writes wait for the window",
          "[http][server][tunnel][http2][flow]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // more than the tunnel's window, from the client; and much more than the client's stream window, from the handler.
    const auto in  = pattern(300'000);
    const auto out = pattern(std::size_t{1} << 20);

    std::atomic<std::uint64_t> go           = 0;     // the client has sent all its window allows (the handler reads)
    std::atomic<std::uint64_t> wrote        = 0;     // the client's write of in has returned
    std::atomic<bool>          reading      = false; // the client has begun to read the handler's write
    std::atomic<bool>          read_whole   = false; // the handler read in with one read, intact
    std::atomic<bool>          write_waited = false; // the handler's write of out returned once the client was reading

    http::router router;
    router.GET("/flow",
               [&](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto switched = co_await http::detail::switch_protocols(rw, upgrade_fields());
                   if (!switched.has_value() || !co_await wait_for_word(&sched, go)) co_return;
                   auto& t = **switched;

                   // larger than the tunnel's buffer: straight from the stream, as it comes.
                   std::string got(in.size(), '\0');
                   const auto  read = co_await t.reader().read(std::as_writable_bytes(std::span{got}));
                   read_whole.store(!read.err && read.count == in.size() && got == in);

                   // larger than the tunnel's buffer: straight to the stream, as the client's window allows.
                   auto written = co_await t.writer().write(std::as_bytes(std::span{out}));
                   if (!written.err) written = co_await t.writer().flush();
                   write_waited.store(!written.err && reading.load());
               });

    auto server = make_server(&sched, std::move(router));

    std::size_t held_in  = 0; // what the client could send before the handler read: the tunnel's window
    std::size_t held_out = 0; // what the handler's write could send before the client read: the client's window
    const auto  result   = sched.run_to_completion(
        over_h2(&sched,
                server->local_port(),
                "/flow",
                [&](h2_tunnels& t, net::http::http2::stream_id id, h2_tunnels::stream& s, tunnel_result& r) -> task<>
                {
                    // the stream's window was widened as the tunnel opened.
                    if (!co_await t.wait_send_window(id, 262'144))
                    {
                        r.err = std::make_error_code(std::errc::timed_out);
                        co_return;
                    }

                    const auto write_in = [](h2_tunnels::stream*         s,
                                             const std::string*          in,
                                             net::io::scheduler*         sched,
                                             std::atomic<std::uint64_t>* wrote) -> task<>
                    {
                        (void)co_await net::io::write_all(*s, *in);
                        wrote->store(1);
                        sched->notify_all(*wrote);
                    };
                    REQUIRE(sched.spawn(write_in(&s, &in, &sched, &wrote)));

                    // the client sends what the window allows, which the server holds unread, then waits for it to
                    // grow.
                    if (co_await t.wait_send_window(id, 0)) held_in = 262'144;
                    go.store(1);
                    sched.notify_all(go);
                    (void)co_await wait_for_word(&sched, wrote);

                    // the handler's write sends what the client's window allows, then waits for it to grow.
                    held_out = co_await t.wait_received(id, 65'535);
                    reading.store(true);
                    std::string got;
                    while (got.size() < out.size())
                    {
                        std::array<std::byte, 16'384> buf{};
                        const auto                    res = co_await s.read(buf);
                        got.append(reinterpret_cast<const char*>(buf.data()), res.count);
                        if (res.err)
                        {
                            r.err = res.err;
                            break;
                        }
                    }
                    r.received = got == out ? "same" : std::format("differs ({} bytes)", got.size());
                }));

    CHECK_FALSE(result.err);
    CHECK(held_in == 262'144);
    CHECK(read_whole.load());
    CHECK(held_out == 65'535);
    CHECK(write_waited.load());
    CHECK(result.received == "same");
}
