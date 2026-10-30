// Opening WebSockets (websocket::connect()): against cppnet's server (echoing, subprotocols, the close handshake either
// way, stopping), and against a scripted server (raw_server, below, reading frames through test/websocket/raw_peer.hpp,
// masked or not): the handshake's request, responses refused, frames right after the 101, the client's frames' masks,
// and the client waiting for the server to end TCP after the close handshake.
#include "http/client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
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
#include "http/connection_pool.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "websocket/error.hpp"
#include "websocket/frame.hpp"
#include "websocket/frames.hpp"
#include "websocket/handshake.hpp"
#include "websocket/raw_peer.hpp"
#include "websocket/websocket.hpp"

namespace http = net::http;
namespace ws   = net::websocket;

namespace
{

using namespace std::chrono_literals;
using namespace std::string_view_literals;

using net::coro::task;
using test::websocket::join;
using test::websocket::raw_peer;
using test::websocket::received_frame;
using ws::detail::opcode;

// record is what handlers report, for the test to check once they've returned.
class record
{
public:
    void add(std::string line)
    {
        std::scoped_lock lock{mu};
        lines.push_back(std::move(line));
    }

    [[nodiscard]] std::vector<std::string> get()
    {
        std::scoped_lock lock{mu};
        return lines;
    }

private:
    std::mutex               mu;
    std::vector<std::string> lines;
};

// describe names an error: a close code with whose it is, or the error's message.
std::string describe(std::error_code err)
{
    if (!err) return "ok";
    if (const auto code = ws::close_code_of(err))
        return std::format("{} {}", static_cast<int>(*code), ws::from_peer(err) ? "peer" : "sent");
    return err.message();
}

std::unique_ptr<http::server> make_server(net::io::scheduler* sched, http::router router)
{
    http::server_config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = "0";

    auto server = std::make_unique<http::server>(sched, std::move(router), cfg);
    REQUIRE(sched->spawn(server->serve()));
    return server;
}

std::string url_of(std::uint16_t port, std::string_view scheme = "ws", std::string_view path = "/ws")
{
    return std::format("{}://127.0.0.1:{}{}", scheme, port, path);
}

// raise sets word to value, waking whoever waits for it.
void raise(net::io::scheduler* sched, std::atomic<std::uint64_t>& word, std::uint64_t value = 1)
{
    word.store(value);
    sched->notify_all(word);
}

// reach waits for word to reach value, for up to 10 s (so a test that goes wrong fails, rather than hangs), returning
// whether it did.
task<bool> reach(net::io::scheduler* sched, const std::atomic<std::uint64_t>& word, std::uint64_t value = 1)
{
    const auto deadline = net::io::clock::now() + 10s;
    while (true)
    {
        const auto seen = word.load();
        if (seen >= value) co_return true;
        if (co_await sched->wait_on(word, seen, deadline)) co_return false;
    }
}

// echo accepts a WebSocket and echoes its messages whole, until a read or write fails, which it records (with the
// subprotocol chosen, first).
// NOTE: options is taken by value, as handlers return echo()'s task rather than await it.
task<void>
echo(const http::server_request& req, http::response_writer& rw, record& out, ws::accept_options options = {})
{
    auto conn = co_await ws::accept(req, rw, options);
    if (!conn.has_value())
    {
        out.add("refused: " + conn.error().message());
        co_return;
    }
    out.add(std::format("subprotocol={}", conn->subprotocol()));

    std::string message;
    while (true)
    {
        const auto type = co_await conn->read_message(message);
        if (!type.has_value())
        {
            out.add(std::format("read: {} reason={}", describe(type.error()), conn->close_reason()));
            co_return;
        }

        if (auto err = co_await conn->write_message(*type, message))
        {
            out.add("write: " + describe(err));
            co_return;
        }
    }
}

// server_frame is a frame as a server sends it: unmasked.
std::vector<std::byte> server_frame(opcode op, std::string_view payload, bool fin = true)
{
    return test::websocket::frame({.fin = fin, .op = op}, test::websocket::bytes(payload));
}

std::vector<std::byte> server_close(ws::close_code code, std::string_view reason = {})
{
    std::array<std::byte, ws::detail::max_control_payload> payload{};
    const auto size = ws::detail::encode_close(code, reason, payload).value_or(0);
    return test::websocket::frame({.fin = true, .op = opcode::close}, std::span{payload}.first(size));
}

// raw_server is a scripted server, on a listener of its own: it accepts one connection and reads its request's head,
// then does what its test says, through a raw_peer (which reads the client's frames, unmasking them).
class raw_server
{
public:
    explicit raw_server(net::io::scheduler* sched)
        : sched{sched}
        , listener{net::listener::make(sched, "127.0.0.1", "0").value()}
    {}

    [[nodiscard]] std::string url(std::string_view path = "/ws") const
    {
        return url_of(listener.local_port(), "ws", path);
    }

    [[nodiscard]] std::uint16_t port() const { return listener.local_port(); }

    // accept accepts the connection and reads the request's head (request).
    task<std::error_code> accept()
    {
        auto accepted = co_await listener.accept();
        if (!accepted.has_value()) co_return accepted.error();

        conn.emplace(std::move(*accepted));
        conn->set_read_timeout(5s);
        peer.emplace(*conn, *conn);

        auto head = co_await peer->read_head();
        if (!head.has_value()) co_return head.error();
        request = std::move(*head);
        co_return std::error_code{};
    }

    // accepting is a 101 accepting the request, with extra lines (each ending in CRLF) before the empty line.
    [[nodiscard]] std::string accepting(std::string_view extra = {}) const
    {
        return std::format("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: {}\r\n{}\r\n",
                           ws::detail::accept_key(key()),
                           extra);
    }

    // key is the request's Sec-WebSocket-Key.
    [[nodiscard]] std::string key() const
    {
        constexpr auto name  = "\r\nSec-WebSocket-Key: "sv;
        const auto     start = request.find(name);
        if (start == std::string::npos) return {};

        const auto from = start + name.size();
        return request.substr(from, request.find("\r\n", from) - from);
    }

    // send sends bytes.
    task<std::error_code> send(std::span<const std::byte> data) { return peer->send(data); }
    task<std::error_code> send(std::string_view text) { return peer->send(text); }

    // until_end reads frames until the client ends the connection, returning how it ended.
    task<std::error_code> until_end()
    {
        auto [frames, err] = co_await peer->read_until_end();
        co_return err;
    }

    // quiet is how a read waiting within for anything from the client ended: std::errc::timed_out if nothing came (the
    // client didn't end the connection, either).
    task<std::error_code> quiet(net::io::clock::duration within)
    {
        conn->set_read_timeout(within);
        auto f = co_await peer->read_frame();
        conn->set_read_timeout(5s);
        co_return f.has_value() ? std::make_error_code(std::errc::bad_message) : f.error();
    }

    net::io::scheduler*            sched;
    net::listener                  listener;
    std::optional<net::tcp_socket> conn;
    std::optional<raw_peer>        peer;
    std::string                    request;
};

// both runs serve (a server's script) as a task of its own, alongside client, until both have finished (or 10 s have
// passed, for a test that goes wrong): serve's results are the test's to have kept.
template<typename Serve, typename Client>
task<void> both(net::io::scheduler* sched, Serve serve, Client client)
{
    // NOTE: shared, as serve's task may outlive this one (if it's still running after the 10 s).
    auto done = std::make_shared<std::atomic<std::uint64_t>>(0);

    const auto run =
        [](Serve serve, net::io::scheduler* sched, std::shared_ptr<std::atomic<std::uint64_t>> done) -> task<void>
    {
        co_await serve();
        raise(sched, *done);
    };
    if (!sched->spawn(run(std::move(serve), sched, done))) co_return;

    co_await client();
    (void)co_await reach(sched, *done);
}

}

TEST_CASE("a WebSocket client echoes messages through cppnet's server, and closes", "[websocket][client]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // ws, or http (the same, as browsers allow), any case.
    const auto scheme = GENERATE("ws"sv, "http"sv, "WS"sv);
    CAPTURE(scheme);

    http::router router;
    router.GET("/ws", [&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); });
    auto server = make_server(&sched, std::move(router));

    http::client client{&sched};

    // a message larger than the client's buffer (16 KiB), which it masks into the buffer a buffer's worth at a time.
    std::string large(300'000, '\0');
    for (std::size_t i = 0; i < large.size(); ++i) large[i] = static_cast<char>('a' + (i % 26));

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;

        auto conn = co_await ws::connect(client, url_of(server->local_port(), scheme));
        if (!conn.has_value()) co_return std::vector<std::string>{"connect: " + conn.error().message()};

        // the WebSocket's connection is its own, not the pool's.
        lines.push_back(std::format("pooled: {}", client.stats().open));

        std::string message;
        for (const auto& [type, data] : {
                 std::pair{  ws::message_type::text,           std::string{"hello"}},
                 std::pair{ws::message_type::binary, std::string{"\x00\x01\xff", 3}},
                 std::pair{  ws::message_type::text,                  std::string{}},
                 std::pair{  ws::message_type::text,                          large}
        })
        {
            if (auto err = co_await conn->write_message(type, data))
                co_return std::vector<std::string>{"write: " + describe(err)};
            const auto read = co_await conn->read_message(message);
            if (!read.has_value()) co_return std::vector<std::string>{"read: " + describe(read.error())};
            lines.push_back(std::format("{} {} {}",
                                        *read == ws::message_type::text ? "text" : "binary",
                                        message.size(),
                                        message == data ? "same" : "differs"));
        }

        // streamed, a frame a write: the large part through the buffer too.
        auto writer = co_await conn->begin_message(ws::message_type::text);
        if (!writer.has_value()) co_return std::vector<std::string>{"begin: " + describe(writer.error())};
        (void)co_await writer->write(std::as_bytes(std::span{"stre"sv}));
        (void)co_await writer->write(std::as_bytes(std::span{large}));
        (void)co_await writer->write(std::as_bytes(std::span{"amed"sv}));
        (void)co_await writer->end_message();
        const auto streamed = co_await conn->read_message(message);
        lines.push_back(std::format("streamed {} {}",
                                    streamed.has_value() ? "ok" : describe(streamed.error()),
                                    message == "stre" + large + "amed" ? "same" : "differs"));

        lines.push_back("close: " + describe(co_await conn->close(static_cast<ws::close_code>(4000), "done")));
        lines.push_back("then: " + describe(co_await conn->write_message(ws::message_type::text, "late"sv)));
        co_return lines;
    };

    const auto lines = sched.run_to_completion(test());
    CHECK(lines
          == std::vector<std::string>{"pooled: 0",
                                      "text 5 same",
                                      "binary 3 same",
                                      "text 0 same",
                                      "text 300000 same",
                                      "streamed ok same",
                                      "close: ok",
                                      "then: 4000 peer"});
    CHECK(out.get() == std::vector<std::string>{"subprotocol=", "read: 4000 peer reason=done"});
}

TEST_CASE("a WebSocket client offers subprotocols, of which the server chooses one", "[websocket][client]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    struct test_case
    {
        std::vector<std::string> offered;
        std::string              chosen;
    };
    const auto tc = GENERATE(
        test_case{
            .offered = {"chat", "chat.v2"},
            .chosen  = "chat.v2"
    },
        test_case{.offered = {"chat"}, .chosen = "chat"},
        test_case{.offered = {"other"}, .chosen = ""},
        test_case{.offered = {}, .chosen = ""});
    CAPTURE(tc.offered);

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw)
               {
                   return echo(req,
                               rw,
                               out,
                               {
                                   .subprotocols = {"chat.v2", "chat"}
                   });
               });
    auto server = make_server(&sched, std::move(router));

    http::client client{&sched};

    const auto test = [&] -> task<std::string>
    {
        auto conn = co_await ws::connect(client, url_of(server->local_port()), {.subprotocols = tc.offered});
        if (!conn.has_value()) co_return "connect: " + conn.error().message();

        std::string chosen{conn->subprotocol()};
        (void)co_await conn->close();
        co_return chosen;
    };

    CHECK(sched.run_to_completion(test()) == tc.chosen);
    CHECK(out.get() == std::vector<std::string>{"subprotocol=" + tc.chosen, "read: 1000 peer reason="});
}

TEST_CASE("the server's close ends a WebSocket client's read with its code", "[websocket][client]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw);
                   if (!conn.has_value()) co_return;

                   (void)co_await conn->write_message(ws::message_type::text, "first"sv);
                   out.add("close: " + describe(co_await conn->close(ws::close_code::going_away, "bye")));
               });
    auto server = make_server(&sched, std::move(router));

    http::client client{&sched};

    const auto test = [&] -> task<std::vector<std::string>>
    {
        auto conn = co_await ws::connect(client, url_of(server->local_port()));
        if (!conn.has_value()) co_return std::vector<std::string>{"connect: " + conn.error().message()};

        std::vector<std::string> lines;
        std::string              message;
        const auto               first = co_await conn->read_message(message);
        lines.push_back(first.has_value() ? message : describe(first.error()));

        // the server's close, answered, as the read's failure; then everything fails so, but close(), which has
        // nothing left to do.
        const auto closed = co_await conn->read_message(message);
        lines.push_back(
            std::format("read: {} reason={}", describe(closed.error_or(std::error_code{})), conn->close_reason()));
        lines.push_back("write: " + describe(co_await conn->write_message(ws::message_type::text, "late"sv)));
        lines.push_back("close: " + describe(co_await conn->close()));
        co_return lines;
    };

    CHECK(sched.run_to_completion(test())
          == std::vector<std::string>{"first", "read: 1001 peer reason=bye", "write: 1001 peer", "close: ok"});
    CHECK(out.get() == std::vector<std::string>{"close: ok"});
}

TEST_CASE("stop ends a WebSocket client's connecting and handshake", "[websocket][client][cancel]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // stopped before connecting, or by the server's handler as it gets the request (so the client waits for the
    // response, which the handler holds back until the client has given up).
    const bool before = GENERATE(true, false);
    CAPTURE(before);

    std::stop_source           stop;
    std::atomic<std::uint64_t> released = 0;

    http::router router;
    router.GET("/ws",
               [&](http::server_request& /*req*/, http::response_writer& /*rw*/) -> task<void>
               {
                   stop.request_stop();
                   out.add(co_await reach(&sched, released) ? "released" : "never released");
               });
    auto server = make_server(&sched, std::move(router));

    http::client client{&sched};

    const auto test = [&] -> task<std::error_code>
    {
        if (before) stop.request_stop();
        auto conn = co_await ws::connect(client, url_of(server->local_port()), {.stop = stop.get_token()});
        raise(&sched, released);
        co_return conn.has_value() ? std::error_code{} : conn.error();
    };

    CHECK(sched.run_to_completion(test()) == std::errc::operation_canceled);
    CHECK(out.get() == (before ? std::vector<std::string>{} : std::vector<std::string>{"released"}));
}

TEST_CASE("stop ends a WebSocket client's read", "[websocket][client][cancel]")
{
    record                     out;
    std::atomic<std::uint64_t> handled = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    std::stop_source stop;

    // the server says it's ready, then requests stop as the client reads on (most likely as it waits).
    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw);
                   if (!conn.has_value()) co_return;

                   (void)co_await conn->write_message(ws::message_type::text, "ready"sv);
                   (void)co_await sched.sleep_until(net::io::clock::now() + 50ms);
                   stop.request_stop();

                   std::string message;
                   const auto  read = co_await conn->read_message(message);
                   out.add("read: " + describe(read.error_or(std::error_code{})));
                   raise(&sched, handled);
               });
    auto server = make_server(&sched, std::move(router));

    http::client client{&sched};

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;
        {
            auto conn = co_await ws::connect(client, url_of(server->local_port()), {.stop = stop.get_token()});
            if (!conn.has_value()) co_return std::vector<std::string>{"connect: " + conn.error().message()};

            std::string message;
            const auto  ready = co_await conn->read_message(message);
            lines.push_back(ready.has_value() ? message : describe(ready.error()));

            const auto read = co_await conn->read_message(message);
            lines.push_back("read: " + describe(read.error_or(std::error_code{})));
            lines.push_back("write: " + describe(co_await conn->write_message(ws::message_type::text, "x"sv)));
            lines.push_back("close: " + describe(co_await conn->close()));
        }

        // the server sees the connection end without a close once it's gone.
        (void)co_await reach(&sched, handled);
        co_return lines;
    };

    const auto canceled = std::make_error_code(std::errc::operation_canceled).message();
    CHECK(sched.run_to_completion(test())
          == std::vector<std::string>{"ready", "read: " + canceled, "write: " + canceled, "close: " + canceled});
    CHECK(out.get() == std::vector<std::string>{"read: 1006 peer"});
}

TEST_CASE("a WebSocket client's handshake request", "[websocket][client][handshake]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    raw_server         server{&sched};
    http::client       client{&sched};

    std::string subprotocol;
    std::string ended;

    ws::connect_options options{
        .subprotocols = {"a", "b"},
        .origin       = "https://example.com"
    };
    options.headers.set("Authorization"sv, "Bearer x"sv);
    options.headers.set("Upgrade"sv, "h2c"sv);
    options.headers.set("Sec-WebSocket-Extensions"sv, "permessage-deflate"sv);

    const auto serve = [&] -> task<void>
    {
        if (co_await server.accept()) co_return;
        (void)co_await server.send(server.accepting("Sec-WebSocket-Protocol: b\r\n"));

        // the client's close, answered, then the end of the stream.
        auto f = co_await server.peer->read_frame();
        if (f.has_value() && f->header.op == opcode::close)
            (void)co_await server.send(server_close(ws::close_code::normal_closure));
        server.conn->shutdown_write();
        ended = describe(co_await server.until_end());
    };

    const auto run = [&] -> task<void>
    {
        auto conn = co_await ws::connect(client, server.url("/chat?room=1#here"), options);
        if (!conn.has_value())
        {
            subprotocol = "connect: " + conn.error().message();
            co_return;
        }
        subprotocol = conn->subprotocol();
        (void)co_await conn->close();
    };

    sched.run_to_completion(both(&sched, serve, run));
    CAPTURE(server.request);

    CHECK(server.request.starts_with("GET /chat?room=1 HTTP/1.1\r\n"));
    CHECK(server.request.contains(std::format("\r\nHost: 127.0.0.1:{}\r\n", server.port())));
    CHECK(server.request.contains("\r\nUpgrade: websocket\r\n"));
    CHECK(server.request.contains("\r\nConnection: Upgrade\r\n"));
    CHECK(server.request.contains("\r\nSec-WebSocket-Version: 13\r\n"));
    CHECK(server.request.contains("\r\nSec-WebSocket-Protocol: a, b\r\n"));
    CHECK(server.request.contains("\r\nOrigin: https://example.com\r\n"));
    CHECK(server.request.contains("\r\nAuthorization: Bearer x\r\n"));
    CHECK_FALSE(server.request.contains("h2c"));
    CHECK_FALSE(server.request.contains("Sec-WebSocket-Extensions"));
    CHECK(server.key().size() == 24);
    CHECK(subprotocol == "b");
    CHECK(ended == make_error_code(net::io::errc::end_of_stream).message());
}

TEST_CASE("a WebSocket client refuses a response that isn't the server's accepting", "[websocket][client][handshake]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    raw_server         server{&sched};
    http::client       client{&sched};

    struct test_case
    {
        std::string_view name;
        std::string      extra{};    // lines of a 101 accepting it
        std::string      response{}; // or a response of its own
        std::error_code  want{};
    };
    const auto tc = GENERATE(
        test_case{.name = "the wrong accept", .response = "wrong", .want = make_error_code(ws::errc::bad_accept)},
        test_case{.name     = "200",
                  .response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
                  .want     = make_error_code(ws::errc::bad_status)},
        test_case{.name     = "a redirect, not followed",
                  .response = "HTTP/1.1 302 Found\r\nLocation: /elsewhere\r\nContent-Length: 0\r\n\r\n",
                  .want     = make_error_code(ws::errc::bad_status)},
        test_case{.name  = "a subprotocol not offered",
                  .extra = "Sec-WebSocket-Protocol: chat\r\n",
                  .want  = make_error_code(ws::errc::bad_subprotocol)},
        test_case{.name  = "an extension not offered",
                  .extra = "Sec-WebSocket-Extensions: permessage-deflate\r\n",
                  .want  = make_error_code(ws::errc::bad_extension)},
        test_case{.name     = "no Upgrade",
                  .response = "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n\r\n",
                  .want     = make_error_code(ws::errc::not_websocket)},
        test_case{.name     = "not HTTP",
                  .response = "SSH-2.0-OpenSSH_9.9\r\n\r\n",
                  .want     = std::make_error_code(std::errc::invalid_argument)},
        test_case{.name     = "the connection ending",
                  .response = "",
                  .want     = make_error_code(net::io::errc::end_of_stream)});
    CAPTURE(tc.name);

    std::string ended;
    const auto  serve = [&] -> task<void>
    {
        if (co_await server.accept()) co_return;

        if (tc.response == "wrong")
        {
            (void)co_await server.send(
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"sv);
        }
        else if (!tc.response.empty() || !tc.extra.empty())
        {
            (void)co_await server.send(tc.response.empty() ? server.accepting(tc.extra) : tc.response);
        }
        else
        {
            server.conn->shutdown_write();
        }

        // the client ends the connection it refused.
        ended = describe(co_await server.until_end());
    };

    std::error_code err;
    const auto      run = [&] -> task<void>
    {
        auto conn = co_await ws::connect(client, server.url(), {.subprotocols = {"superchat"}});
        err       = conn.has_value() ? std::error_code{} : conn.error();
    };

    sched.run_to_completion(both(&sched, serve, run));
    CAPTURE(err.message());
    CHECK(err == tc.want);
    CHECK(ended == make_error_code(net::io::errc::end_of_stream).message());
}

TEST_CASE("a WebSocket client's connecting and handshake fail as connecting does", "[websocket][client][handshake]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // a port no one listens on: a listener's, closed.
    std::uint16_t closed_port = 0;
    {
        auto listener = net::listener::make(&sched, "127.0.0.1", "0").value();
        closed_port   = listener.local_port();
    }

    // a server that never answers.
    raw_server   server{&sched};
    http::client client{&sched, {.response_header_timeout = 200ms}};

    const auto serve = [&] -> task<void>
    {
        if (co_await server.accept()) co_return;
        (void)co_await server.until_end();
    };

    std::vector<std::string> lines;
    const auto               run = [&] -> task<void>
    {
        for (const auto& url : {std::string{"ftp://127.0.0.1/ws"},
                                std::string{"127.0.0.1/ws"},
                                std::string{"ws://[::1/ws"},
                                url_of(closed_port),
                                server.url()})
        {
            auto conn = co_await ws::connect(client, url);
            lines.push_back(conn.has_value() ? "ok" : conn.error().message());
        }
    };

    sched.run_to_completion(both(&sched, serve, run));
    CHECK(lines
          == std::vector<std::string>{std::make_error_code(std::errc::protocol_not_supported).message(),
                                      std::make_error_code(std::errc::protocol_not_supported).message(),
                                      std::make_error_code(std::errc::invalid_argument).message(),
                                      std::make_error_code(std::errc::connection_refused).message(),
                                      std::make_error_code(std::errc::timed_out).message()});
}

TEST_CASE("a WebSocket client reads the frames sent right after the 101", "[websocket][client]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    raw_server         server{&sched};
    http::client       client{&sched};

    std::vector<std::string> received;
    const auto               serve = [&] -> task<void>
    {
        if (co_await server.accept()) co_return;

        // in one write with the 101.
        (void)co_await server.send(join(test::websocket::bytes(server.accepting()),
                                        server_frame(opcode::text, "ear"sv, false),
                                        server_frame(opcode::ping, "p"sv),
                                        server_frame(opcode::continuation, "ly"sv),
                                        server_frame(opcode::binary, "second"sv)));

        // the pong, and the close, answered; then the end of the stream.
        while (true)
        {
            auto f = co_await server.peer->read_frame();
            if (!f.has_value()) break;
            received.push_back(std::format(
                "{}{}:{}",
                f->header.masked ? "" : "unmasked ",
                f->header.op == opcode::pong ? "pong" : "close",
                f->header.op == opcode::close ? std::to_string(static_cast<int>(f->close().code)) : f->text()));
            if (f->header.op == opcode::close)
            {
                (void)co_await server.send(server_close(ws::close_code::normal_closure));
                server.conn->shutdown_write();
                break;
            }
        }
        (void)co_await server.until_end();
    };

    std::vector<std::string> lines;
    const auto               run = [&] -> task<void>
    {
        auto conn = co_await ws::connect(client, server.url());
        if (!conn.has_value())
        {
            lines.push_back("connect: " + conn.error().message());
            co_return;
        }

        std::string message;
        for (int i = 0; i < 2; ++i)
        {
            const auto read = co_await conn->read_message(message);
            lines.push_back(read.has_value() ? message : describe(read.error()));
        }
        lines.push_back("close: " + describe(co_await conn->close()));
    };

    sched.run_to_completion(both(&sched, serve, run));
    CHECK(lines == std::vector<std::string>{"early", "second", "close: ok"});
    CHECK(received == std::vector<std::string>{"pong:p", "close:1000"});
}

TEST_CASE("a WebSocket client masks its frames, each with a key of its own, leaving the data as it was",
          "[websocket][client][mask]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    raw_server         server{&sched};
    http::client       client{&sched};

    // a small message, one larger than the client's buffer, and a streamed one of small and large frames.
    const std::string small = "hello";
    std::string       large(40'000, '\0');
    for (std::size_t i = 0; i < large.size(); ++i) large[i] = static_cast<char>(i * 13);
    const std::string large_copy = large;
    std::string       part(20'000, 'p');

    std::vector<received_frame> frames;
    const auto                  serve = [&] -> task<void>
    {
        if (co_await server.accept()) co_return;
        (void)co_await server.send(server.accepting());

        while (true)
        {
            auto f = co_await server.peer->read_frame();
            if (!f.has_value()) break;
            frames.push_back(std::move(*f));
            if (frames.back().header.op == opcode::close)
            {
                (void)co_await server.send(server_close(ws::close_code::normal_closure));
                server.conn->shutdown_write();
                break;
            }
        }
        (void)co_await server.until_end();
    };

    std::vector<std::string> lines;
    const auto               run = [&] -> task<void>
    {
        auto conn = co_await ws::connect(client, server.url());
        if (!conn.has_value())
        {
            lines.push_back("connect: " + conn.error().message());
            co_return;
        }

        lines.push_back(describe(co_await conn->write_message(ws::message_type::text, small)));
        lines.push_back(describe(co_await conn->write_message(ws::message_type::binary, large)));

        auto writer = co_await conn->begin_message(ws::message_type::binary);
        if (writer.has_value())
        {
            lines.push_back(describe((co_await writer->write(std::as_bytes(std::span{small}))).err));
            lines.push_back(describe((co_await writer->write(std::as_bytes(std::span{part}))).err));
            lines.push_back(describe(co_await writer->end_message()));
        }

        const std::array<std::byte, 2> ping{std::byte{1}, std::byte{2}};
        lines.push_back(describe(co_await conn->ping(ping)));
        lines.push_back(describe(co_await conn->close()));
    };

    sched.run_to_completion(both(&sched, serve, run));
    CHECK(lines == std::vector<std::string>(7, "ok"));

    // the caller's data, as it was.
    CHECK(large == large_copy);
    CHECK(part == std::string(20'000, 'p'));

    REQUIRE(frames.size() == 7);
    CHECK(frames[0].text() == small);
    CHECK(frames[1].payload == test::websocket::bytes(large));
    CHECK(frames[2].text() == small);
    CHECK(frames[3].text() == part);
    CHECK(frames[4].payload.empty());
    CHECK(frames[5].header.op == opcode::ping);
    CHECK(frames[6].header.op == opcode::close);

    std::set<std::array<std::byte, 4>> keys;
    for (const auto& f : frames)
    {
        CHECK(f.header.masked);
        keys.insert(f.header.mask_key);
    }
    CHECK(keys.size() == frames.size());
}

TEST_CASE("a WebSocket client ends TCP only after the server, once the close handshake is done",
          "[websocket][client][close]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    raw_server         server{&sched};
    http::client       client{&sched};

    // whose close comes first: the client's (close(), alone, or while another task reads, which then gets the
    // server's close), or the server's (which the client's read answers, the server sending more after it, which the
    // client drops; and again with close() called once the read has answered, as it waits for the server's end, which
    // close() then waits for too); and whether the server then ends TCP (after a while, in which the client must not),
    // or never does, so the client does once close_timeout has passed.
    enum class first : std::uint8_t
    {
        client,
        client_reading,
        server,
        server_then_close,
    };
    const auto who         = GENERATE(first::client, first::client_reading, first::server, first::server_then_close);
    const bool server_ends = GENERATE(true, false);
    CAPTURE(static_cast<int>(who), server_ends);

    // (long enough, when the server ends TCP, not to be reached.)
    const auto     close_timeout = server_ends ? 2'000ms : 300ms;
    constexpr auto pause         = 100ms;

    const bool server_first = who == first::server || who == first::server_then_close;

    std::atomic<std::uint64_t> answered = 0; // the server has the client's answer to its close
    std::vector<std::string>   served;
    const auto                 serve = [&] -> task<void>
    {
        if (co_await server.accept()) co_return;
        (void)co_await server.send(server.accepting());
        if (server_first)
        {
            // more than the client's buffer holds, after the close.
            (void)co_await server.send(join(server_close(ws::close_code::normal_closure, "bye"),
                                            server_frame(opcode::binary, std::string(20'000, 'j'))));
        }

        auto f = co_await server.peer->read_frame();
        if (!f.has_value() || f->header.op != opcode::close)
        {
            served.emplace_back("no close");
            co_return;
        }
        served.push_back(std::format("close:{}", static_cast<int>(f->close().code)));
        if (server_first) raise(&sched, answered);
        else (void)co_await server.send(server_close(ws::close_code::normal_closure));

        // the client doesn't end the connection first.
        served.push_back("then: " + describe(co_await server.quiet(pause)));
        if (server_ends) server.conn->shutdown_write();

        served.push_back("end: " + describe(co_await server.until_end()));
    };

    std::vector<std::string> lines;
    net::io::clock::duration took{};
    net::io::clock::duration close_took{};
    const auto               run = [&] -> task<void>
    {
        auto conn = co_await ws::connect(client, server.url(), {.connection = {.close_timeout = close_timeout}});
        if (!conn.has_value())
        {
            lines.push_back("connect: " + conn.error().message());
            co_return;
        }

        const auto  start = net::io::clock::now();
        std::string message;
        switch (who)
        {
        case first::client: lines.push_back("close: " + describe(co_await conn->close())); break;

        case first::client_reading:
        {
            // NOTE: read_message() claims the read side as it's called, so the close is sure to find it taken.
            auto reading = conn->read_message(message);

            std::error_code            closed;
            std::atomic<std::uint64_t> done   = 0;
            const auto                 closer = [](net::io::scheduler*         sched,
                                                   ws::connection*             conn,
                                                   std::error_code*            closed,
                                                   std::atomic<std::uint64_t>* done) -> task<void>
            {
                *closed = co_await conn->close();
                raise(sched, *done);
            };
            if (!sched.spawn(closer(&sched, &*conn, &closed, &done))) co_return;

            const auto read = co_await std::move(reading);
            (void)co_await reach(&sched, done);
            took = net::io::clock::now() - start;

            lines.push_back("read: " + describe(read.error_or(std::error_code{})));
            lines.push_back("close: " + describe(closed));
            co_return;
        }

        case first::server:
        {
            const auto read = co_await conn->read_message(message);
            lines.push_back("read: " + describe(read.error_or(std::error_code{})));
            break;
        }

        case first::server_then_close:
        {
            std::error_code            closed;
            std::atomic<std::uint64_t> done   = 0;
            const auto                 closer = [](net::io::scheduler*               sched,
                                                   ws::connection*                   conn,
                                                   const std::atomic<std::uint64_t>* answered,
                                                   std::error_code*                  closed,
                                                   net::io::clock::duration*         took,
                                                   std::atomic<std::uint64_t>*       done) -> task<void>
            {
                (void)co_await reach(sched, *answered);
                const auto start = net::io::clock::now();
                *closed          = co_await conn->close();
                *took            = net::io::clock::now() - start;
                raise(sched, *done);
            };
            if (!sched.spawn(closer(&sched, &*conn, &answered, &closed, &close_took, &done))) co_return;

            const auto read = co_await conn->read_message(message);
            (void)co_await reach(&sched, done);
            took = net::io::clock::now() - start;

            lines.push_back("read: " + describe(read.error_or(std::error_code{})));
            lines.push_back("close: " + describe(closed));
            co_return;
        }
        }
        took = net::io::clock::now() - start;
    };

    sched.run_to_completion(both(&sched, serve, run));
    switch (who)
    {
    case first::client: CHECK(lines == std::vector<std::string>{"close: ok"}); break;
    case first::client_reading: CHECK(lines == std::vector<std::string>{"read: 1000 peer", "close: ok"}); break;
    case first::server: CHECK(lines == std::vector<std::string>{"read: 1000 peer"}); break;
    case first::server_then_close:
        CHECK(lines == std::vector<std::string>{"read: 1000 peer", "close: ok"});

        // close() came as the server held back its end (pause), and returned with it, or with close_timeout.
        CHECK(close_took >= (server_ends ? pause / 2 : close_timeout / 2));
        break;
    }
    CHECK(served
          == std::vector<std::string>{"close:1000",
                                      "then: " + std::make_error_code(std::errc::timed_out).message(),
                                      "end: " + make_error_code(net::io::errc::end_of_stream).message()});

    // it returned once the server had ended TCP, or once it had waited close_timeout for it.
    CHECK(took >= (server_ends ? pause : close_timeout));
    if (server_ends) CHECK(took < close_timeout);
}

TEST_CASE("a WebSocket client's writes time out at the client's io_timeout", "[websocket][client]")
{
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};
    raw_server                 server{&sched};
    http::client               client{&sched, {.io_timeout = 200ms}};
    std::atomic<std::uint64_t> client_done = 0;

    // a server that doesn't read, until the client's done.
    const auto serve = [&] -> task<void>
    {
        if (co_await server.accept()) co_return;
        (void)co_await server.send(server.accepting());
        (void)co_await reach(&sched, client_done);
        (void)co_await server.until_end();
    };

    std::vector<std::string> lines;
    const auto               run = [&] -> task<void>
    {
        {
            auto conn = co_await ws::connect(client, server.url());
            if (!conn.has_value())
            {
                lines.push_back("connect: " + conn.error().message());
            }
            else
            {
                // a read waiting meanwhile, which the failure ends (aborting the connection).
                // NOTE: read_message() claims the read side as it's called.
                std::string message;
                auto        reading = conn->read_message(message);

                std::expected<ws::message_type, std::error_code> read;
                std::atomic<std::uint64_t>                       read_done = 0;
                const auto reader = [](net::io::scheduler*                                    sched,
                                       task<std::expected<ws::message_type, std::error_code>> reading,
                                       std::expected<ws::message_type, std::error_code>*      read,
                                       std::atomic<std::uint64_t>*                            done) -> task<void>
                {
                    *read = co_await std::move(reading);
                    raise(sched, *done);
                };
                if (!sched.spawn(reader(&sched, std::move(reading), &read, &read_done))) co_return;

                // until the connection's buffers are full, and a write waits io_timeout for the server to read.
                const std::string large(std::size_t{1} << 20, 'x');
                std::error_code   err;
                for (int i = 0; i < 256 && !err; ++i)
                    err = co_await conn->write_message(ws::message_type::binary, large);

                lines.push_back("write: " + describe(err));
                lines.push_back("then: " + describe(co_await conn->write_message(ws::message_type::text, "x"sv)));
                const bool ended = co_await reach(&sched, read_done);
                lines.push_back(ended ? "read: " + describe(read.error_or(std::error_code{})) : "read: still waiting");
            }
        }
        raise(&sched, client_done);
    };

    sched.run_to_completion(both(&sched, serve, run));
    const auto timed_out = std::make_error_code(std::errc::timed_out).message();
    CHECK(lines == std::vector<std::string>{"write: " + timed_out, "then: " + timed_out, "read: " + timed_out});
}
