// Opening WebSockets over HTTP/2 (RFC 8441): websocket::connect() opens an extended CONNECT's stream on the client's
// HTTP/2 connection to the host, if it has one whose server allows it with a stream free, and otherwise connects over
// HTTP/1.1 as before. Against cppnet's server (over cleartext with prior knowledge: TLS's ALPN is
// test/tls/server.cpp's), and against a scripted server (test::h2_client as the server), which shows the request,
// answers it as each test says, and sees how the client ends its stream.
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
#include "http/client.hpp"
#include "http/connection_pool.hpp"
#include "http/h2_client.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
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
#include "websocket/error.hpp"
#include "websocket/frame.hpp"
#include "websocket/frames.hpp"
#include "websocket/handshake.hpp"
#include "websocket/raw_peer.hpp"
#include "websocket/websocket.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;
namespace ws    = net::websocket;

namespace
{

using namespace std::chrono_literals;
using namespace std::string_view_literals;

using net::coro::task;
using test::websocket::raw_peer;
using ws::detail::opcode;

// record is what handlers (and scripted servers) report, for the test to check once they've returned.
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

// prior_knowledge is a client's options that speak HTTP/2 to http:// URLs.
http::client_options prior_knowledge()
{
    http::client_options options;
    options.http2_prior_knowledge = true;
    return options;
}

// make_server is cppnet's server on loopback, configured by config (its host and port set here).
std::unique_ptr<http::server>
make_server(net::io::scheduler* sched, http::router router, http::server_config config = {})
{
    config.host = "127.0.0.1";
    config.port = "0";

    auto server = std::make_unique<http::server>(sched, std::move(router), config);
    REQUIRE(sched->spawn(server->serve()));
    return server;
}

std::string url_of(std::uint16_t port, std::string_view scheme = "ws", std::string_view path = "/ws")
{
    return std::format("{}://127.0.0.1:{}{}", scheme, port, path);
}

// fetch sends a GET for url and reads its response, returning its status and body (or why it failed).
task<std::string> fetch(http::client& client, std::string url)
{
    auto resp = co_await client.get(url);
    if (!resp.has_value()) co_return "get: " + resp.error().message();

    auto body = co_await net::io::read_all(*resp->body);
    co_return std::format("{} {}", static_cast<int>(resp->status_code), body.value_or("(failed)"));
}

// routes is a router with GET / answering "ok", and GET /ws handled by ws_handler.
template<typename Handler>
http::router routes(Handler ws_handler)
{
    http::router router;
    router.GET("/",
               [](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto* body = co_await rw.send(http::status::OK, 2);
                   (void)co_await net::io::write_all(*body, "ok"sv);
               });
    router.GET("/ws", std::move(ws_handler));
    return router;
}

// echo accepts a WebSocket (recording the HTTP version it came over, and the subprotocol chosen) and echoes its
// messages whole, until a read or write fails, which it records; then raises done, if it's given.
task<void> echo(const http::server_request& req,
                http::response_writer&      rw,
                record&                     out,
                ws::accept_options          options = {},
                std::atomic<std::uint64_t>* done    = nullptr,
                net::io::scheduler*         sched   = nullptr)
{
    auto conn = co_await ws::accept(req, rw, options);
    if (!conn.has_value())
    {
        out.add(std::format("HTTP/{} refused: {}", req.version.major, conn.error().message()));
        co_return;
    }
    out.add(std::format("HTTP/{} subprotocol={}", req.version.major, conn->subprotocol()));

    std::string message;
    while (true)
    {
        const auto type = co_await conn->read_message(message);
        if (!type.has_value())
        {
            out.add(std::format("read: {} reason={}", describe(type.error()), conn->close_reason()));
            break;
        }

        if (auto err = co_await conn->write_message(*type, message))
        {
            out.add("write: " + describe(err));
            break;
        }
    }

    if (done != nullptr) raise(sched, *done, done->load() + 1);
}

// echoes writes a message and reads its echo, returning "same", or what went wrong.
task<std::string> echoes(ws::connection& conn, std::string_view data)
{
    if (auto err = co_await conn.write_message(ws::message_type::text, data)) co_return "write: " + describe(err);

    std::string message;
    const auto  read = co_await conn.read_message(message);
    if (!read.has_value()) co_return "read: " + describe(read.error());
    co_return message == data ? "same" : "differs";
}

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

// played is a connection a scripted server accepted, and the HTTP/2 server it plays on it (test::h2_client as the
// server), which allows extended CONNECT (SETTINGS_ENABLE_CONNECT_PROTOCOL = 1), as cppnet's server does.
struct played
{
    static http2::session_config allowing()
    {
        http2::session_config config;
        config.local.enable_connect_protocol = true;
        return config;
    }

    explicit played(net::tcp_socket accepted)
        : socket{std::move(accepted)}
        , server{&socket, &socket, &socket, http2::role::server, allowing()}
    {}

    net::tcp_socket socket;
    test::h2_client server;
};

// script is a scripted HTTP/2 server's listener, whose first connection the client makes with a GET (with prior
// knowledge), answered "ok", so the client has an HTTP/2 connection to it for the WebSocket to use.
struct script
{
    explicit script(net::io::scheduler* sched)
        : listener{net::listener::make(sched, "127.0.0.1", "0").value()}
    {}

    [[nodiscard]] std::uint16_t port() const { return listener.local_port(); }

    // open accepts the client's connection and answers its GET (stream 1), then waits for its extended CONNECT's head
    // (stream 3), returning whether it came.
    task<bool> open()
    {
        auto accepted = co_await listener.accept();
        if (!accepted.has_value()) co_return false;
        conn = std::make_unique<played>(std::move(*accepted));

        auto&      s   = conn->server;
        const bool get = co_await s.pump([&s] { return s.responses[1].done; });
        if (!get) co_return false;
        s.respond(1, "200", "ok");
        co_return co_await s.pump([&s] { return !s.responses[3].headers.empty(); });
    }

    // frames are the WebSocket frames in what the client's sent on stream 3 so far (unmasked), but a frame cut short.
    [[nodiscard]] std::vector<test::websocket::received_frame> frames() const
    {
        const auto&            body = conn->server.responses[3].body;
        std::vector<std::byte> data{reinterpret_cast<const std::byte*>(body.data()),
                                    reinterpret_cast<const std::byte*>(body.data()) + body.size()};

        std::vector<test::websocket::received_frame> out;
        std::span<const std::byte>                   rest{data};
        while (true)
        {
            const auto decoded = ws::detail::decode_header(rest);
            if (!decoded || !*decoded || rest.size() - (*decoded)->size < (*decoded)->header.length) return out;

            const auto size   = (*decoded)->size;
            const auto length = static_cast<std::size_t>((*decoded)->header.length);
            auto&      f      = out.emplace_back();
            f.header          = (*decoded)->header;
            f.payload.assign(rest.begin() + static_cast<std::ptrdiff_t>(size),
                             rest.begin() + static_cast<std::ptrdiff_t>(size + length));
            if (f.header.masked) ws::detail::apply_mask(f.payload, f.header.mask_key, 0);
            rest = rest.subspan(size + length);
        }
    }

    // send sends a server's frame on stream 3, ending the stream if end says.
    void send(std::span<const std::byte> frame, bool end = false) const
    {
        conn->server.send(3, {reinterpret_cast<const char*>(frame.data()), frame.size()}, end);
    }

    net::listener           listener;
    std::unique_ptr<played> conn;
};

// server_frame is a frame as a server sends it: unmasked.
std::vector<std::byte> server_frame(opcode op, std::string_view payload)
{
    return test::websocket::frame({.fin = true, .op = op}, test::websocket::bytes(payload));
}

std::vector<std::byte> server_close(ws::close_code code)
{
    std::array<std::byte, ws::detail::max_control_payload> payload{};
    const auto size = ws::detail::encode_close(code, {}, payload).value_or(0);
    return test::websocket::frame({.fin = true, .op = opcode::close}, std::span{payload}.first(size));
}

// connect_after_get sends a GET to the scripted server (so the client has an HTTP/2 connection to it), then opens a
// WebSocket to it.
task<std::expected<ws::connection, std::error_code>>
connect_after_get(http::client& client, std::uint16_t port, ws::connect_options options = {})
{
    const auto got = co_await fetch(client, url_of(port, "http", "/"));
    if (got != "200 ok") co_return std::unexpected(std::make_error_code(std::errc::protocol_error));
    co_return co_await ws::connect(client, url_of(port, "ws", "/ws?x=1"), options);
}

}

TEST_CASE("a WebSocket client opens a stream of its HTTP/2 connection, which goes on with requests beside it",
          "[websocket][client][http2]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    std::uint16_t port = 0;
    auto          server =
        make_server(&sched,
                    routes(
                        [&](http::server_request& req, http::response_writer& rw) -> task<void>
                        {
                            // what the extended CONNECT came with, as the server presents it.
                            out.add(std::format("{} protocol={}", http::method_string(req.method), req.protocol));
                            for (const auto name : {"Authorization"sv,
                                                    "Origin"sv,
                                                    "Sec-WebSocket-Protocol"sv,
                                                    "Sec-WebSocket-Key"sv,
                                                    "Sec-WebSocket-Extensions"sv,
                                                    "Upgrade"sv})
                            {
                                if (auto v = req.headers.get(name)) out.add(std::format("{}: {}", name, *v));
                            }
                            co_await echo(req, rw, out, {.subprotocols = {"b"}});
                        }));
    port = server->local_port();

    http::client client{&sched, prior_knowledge()};

    ws::connect_options options{
        .subprotocols = {"a", "b"},
        .origin       = std::format("http://127.0.0.1:{}", port),
    };
    options.headers.set("Authorization"sv, "Bearer x"sv);
    options.headers.set("Upgrade"sv, "h2c"sv);
    options.headers.set("Sec-WebSocket-Extensions"sv, "permessage-deflate"sv);
    options.headers.set("Sec-WebSocket-Key"sv, "not one"sv);

    // a message larger than the stream's windows and the tunnel's buffers.
    std::string large(300'000, '\0');
    for (std::size_t i = 0; i < large.size(); ++i) large[i] = static_cast<char>('a' + (i % 26));

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;
        lines.push_back(co_await fetch(client, url_of(port, "http", "/")));

        auto conn = co_await ws::connect(client, url_of(port), options);
        if (!conn.has_value()) co_return std::vector<std::string>{"connect: " + conn.error().message()};
        lines.push_back(std::format("subprotocol={} open={}", conn->subprotocol(), client.stats().open));

        for (const auto& data : {std::string{"hello"}, std::string{}, large})
            lines.push_back(co_await echoes(*conn, data));

        // streamed, a frame a write.
        auto writer = co_await conn->begin_message(ws::message_type::binary);
        if (!writer.has_value()) co_return std::vector<std::string>{"begin: " + describe(writer.error())};
        (void)co_await writer->write(std::as_bytes(std::span{"stre"sv}));
        (void)co_await writer->write(std::as_bytes(std::span{large}));
        (void)co_await writer->end_message();
        std::string message;
        const auto  streamed = co_await conn->read_message(message);
        lines.push_back(std::format("streamed {} {}",
                                    streamed.has_value() ? "ok" : describe(streamed.error()),
                                    message == "stre" + large ? "same" : "differs"));

        // a request on the same connection, while the WebSocket's open.
        lines.push_back(co_await fetch(client, url_of(port, "http", "/")));

        lines.push_back("close: " + describe(co_await conn->close(static_cast<ws::close_code>(4000), "done")));
        lines.push_back("then: " + describe(co_await conn->write_message(ws::message_type::text, "late"sv)));

        // and once it's closed: still the one connection.
        lines.push_back(co_await fetch(client, url_of(port, "http", "/")));
        lines.push_back(std::format("open={}", client.stats().open));
        co_return lines;
    };

    const auto lines = sched.run_to_completion(test());
    CHECK(lines
          == std::vector<std::string>{"200 ok",
                                      "subprotocol=b open=1",
                                      "same",
                                      "same",
                                      "same",
                                      "streamed ok same",
                                      "200 ok",
                                      "close: ok",
                                      "then: 4000 peer",
                                      "200 ok",
                                      "open=1"});
    CHECK(out.get()
          == std::vector<std::string>{"GET protocol=websocket",
                                      "Authorization: Bearer x",
                                      std::format("Origin: http://127.0.0.1:{}", port),
                                      "Sec-WebSocket-Protocol: a, b",
                                      "HTTP/2 subprotocol=b",
                                      "read: 4000 peer reason=done"});
}

TEST_CASE("a WebSocket client uses HTTP/1.1, on a connection of its own, without an HTTP/2 connection that allows it",
          "[websocket][client][http2]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    struct test_case
    {
        std::string name;
        bool        client_http2 = true; // connect_options::http2
        bool        websockets   = true; // the server's http2_websockets
        bool        warm         = true; // the client has an HTTP/2 connection (a GET's) to the server
        int         version      = 2;    // what the WebSocket's over
    };
    const auto tc = GENERATE(test_case{.name = "an HTTP/2 connection that allows it"},
                             test_case{.name = "connect_options::http2 off", .client_http2 = false, .version = 1},
                             test_case{.name = "a server that doesn't allow it", .websockets = false, .version = 1},
                             test_case{.name = "no HTTP/2 connection yet", .warm = false, .version = 1});
    CAPTURE(tc.name);

    http::server_config config;
    config.http2_websockets = tc.websockets;
    auto server =
        make_server(&sched,
                    routes([&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); }),
                    config);

    http::client client{&sched, prior_knowledge()};

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;
        if (tc.warm) lines.push_back(co_await fetch(client, url_of(server->local_port(), "http", "/")));

        auto conn = co_await ws::connect(client, url_of(server->local_port()), {.http2 = tc.client_http2});
        if (!conn.has_value()) co_return std::vector<std::string>{"connect: " + conn.error().message()};

        lines.push_back(co_await echoes(*conn, "hello"));
        lines.push_back("close: " + describe(co_await conn->close()));

        // the WebSocket's own connection isn't the pool's.
        lines.push_back(std::format("open={}", client.stats().open));
        co_return lines;
    };

    const auto* const open = tc.warm ? "open=1" : "open=0";
    auto              want = std::vector<std::string>{"same", "close: ok", open};
    if (tc.warm) want.insert(want.begin(), "200 ok");
    CHECK(sched.run_to_completion(test()) == want);
    CHECK(out.get()
          == std::vector<std::string>{std::format("HTTP/{} subprotocol=", tc.version), "read: 1000 peer reason="});
}

TEST_CASE("a WebSocket client uses HTTP/1.1 when its HTTP/2 connection has no stream free, rather than wait",
          "[websocket][client][http2]")
{
    record                     out;
    std::atomic<std::uint64_t> ended = 0; // the server's WebSockets that have ended
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    // one stream at a time.
    http::server_config config;
    config.http2_max_concurrent_streams = 1;
    auto server                         = make_server(&sched,
                                                      routes([&](http::server_request& req, http::response_writer& rw)
                                                             { return echo(req, rw, out, {}, &ended, &sched); }),
                                                      config);

    http::client client{&sched, prior_knowledge()};

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;
        lines.push_back(co_await fetch(client, url_of(server->local_port(), "http", "/")));

        {
            // the first takes the connection's one stream, so the second goes over HTTP/1.1.
            auto first = co_await ws::connect(client, url_of(server->local_port()));
            if (!first.has_value()) co_return std::vector<std::string>{"first: " + first.error().message()};
            lines.push_back(co_await echoes(*first, "one"));

            auto second = co_await ws::connect(client, url_of(server->local_port()));
            if (!second.has_value()) co_return std::vector<std::string>{"second: " + second.error().message()};
            lines.push_back(co_await echoes(*second, "two"));

            lines.push_back("close: " + describe(co_await second->close()));
            lines.push_back("close: " + describe(co_await first->close()));
        }

        // once the first's stream is done with (on both sides), the next takes it again.
        const bool both_ended = co_await reach(&sched, ended, 2);
        lines.emplace_back(both_ended ? "ended" : "not ended");

        auto third = co_await ws::connect(client, url_of(server->local_port()));
        if (!third.has_value()) co_return std::vector<std::string>{"third: " + third.error().message()};
        lines.push_back(co_await echoes(*third, "three"));
        lines.push_back("close: " + describe(co_await third->close()));
        co_return lines;
    };

    CHECK(
        sched.run_to_completion(test())
        == std::vector<std::string>{"200 ok", "same", "same", "close: ok", "close: ok", "ended", "same", "close: ok"});
    CHECK(out.get()
          == std::vector<std::string>{"HTTP/2 subprotocol=",
                                      "HTTP/1 subprotocol=",
                                      "read: 1000 peer reason=",
                                      "read: 1000 peer reason=",
                                      "HTTP/2 subprotocol=",
                                      "read: 1000 peer reason="});
}

TEST_CASE("a WebSocket client doesn't use an HTTP/2 connection whose server's SETTINGS haven't come",
          "[websocket][client][http2]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();
    const auto         port     = listener.local_port();

    http::client client{&sched, prior_knowledge()};

    std::atomic<std::uint64_t> get_done = 0;
    std::string                got;
    std::stop_source           get_stop;

    // (stopped at the end: as a request whose connection drops before any response, it would be sent again.)
    const auto get = [&] -> task<void>
    {
        auto resp = co_await client.get(url_of(port, "http", "/"), get_stop.get_token());
        got       = resp.has_value() ? "answered" : resp.error().message();
        raise(&sched, get_done);
    };

    // the first connection (the GET's, over HTTP/2) never hears from its server, which reads only the client's
    // preface: the WebSocket goes over HTTP/1.1 on a second, which the server accepts.
    const auto test = [&] -> task<void>
    {
        if (!sched.spawn(get()))
        {
            out.add("spawn failed");
            co_return;
        }

        auto first = co_await listener.accept();
        if (!first.has_value())
        {
            out.add("accept: " + first.error().message());
            co_return;
        }
        std::array<std::byte, 24> preface{};
        std::size_t               read = 0;
        while (read < preface.size())
        {
            const auto res = co_await first->read(std::span{preface}.subspan(read));
            if (res.err)
            {
                out.add("preface: " + res.err.message());
                co_return;
            }
            read += res.count;
        }

        const auto serve = [&] -> task<void>
        {
            auto second = co_await listener.accept();
            if (!second.has_value()) co_return;

            raw_peer   peer{*second, *second};
            const auto head = co_await peer.read_head();
            out.add(head.has_value() ? head->substr(0, head->find("\r\n")) : head.error().message());

            const auto key = [&]
            {
                constexpr auto name  = "\r\nSec-WebSocket-Key: "sv;
                const auto     start = head.value_or("").find(name);
                if (start == std::string::npos) return std::string{};
                const auto from = start + name.size();
                return head->substr(from, head->find("\r\n", from) - from);
            }();
            (void)co_await peer.send(std::format("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                                 "Connection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
                                                 ws::detail::accept_key(key)));

            // the client's close, answered, then the end.
            auto f = co_await peer.read_frame();
            if (f.has_value()) out.add(f->header.op == opcode::close ? "close" : "not a close");
            (void)co_await peer.send(server_close(ws::close_code::normal_closure));
            second->shutdown_write();
        };

        const auto run = [&] -> task<void>
        {
            auto conn = co_await ws::connect(client, url_of(port));
            out.add(conn.has_value() ? "connected" : "connect: " + conn.error().message());
            if (conn.has_value()) out.add("close: " + describe(co_await conn->close()));
        };

        co_await both(&sched, serve, run);

        // the GET never had an answer.
        get_stop.request_stop();
        (void)co_await reach(&sched, get_done);
    };

    sched.run_to_completion(test());
    CHECK(out.get() == std::vector<std::string>{"GET /ws HTTP/1.1", "connected", "close", "close: ok"});
    CHECK(got == std::make_error_code(std::errc::operation_canceled).message());
}

TEST_CASE("a WebSocket client over HTTP/2 refused by the server fails with bad_status, the connection going on",
          "[websocket][client][http2]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // another origin's refused (403).
    auto server =
        make_server(&sched,
                    routes([&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); }));

    http::client client{&sched, prior_knowledge()};

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;
        lines.push_back(co_await fetch(client, url_of(server->local_port(), "http", "/")));

        auto conn = co_await ws::connect(client, url_of(server->local_port()), {.origin = "https://elsewhere.example"});
        lines.push_back(conn.has_value() ? "connected" : conn.error().message());

        lines.push_back(co_await fetch(client, url_of(server->local_port(), "http", "/")));
        lines.push_back(std::format("open={}", client.stats().open));
        co_return lines;
    };

    CHECK(sched.run_to_completion(test())
          == std::vector<std::string>{"200 ok", make_error_code(ws::errc::bad_status).message(), "200 ok", "open=1"});
    CHECK(out.get()
          == std::vector<std::string>{"HTTP/2 refused: " + make_error_code(ws::errc::origin_refused).message()});
}

TEST_CASE("the server's close and abort reach a WebSocket client over HTTP/2", "[websocket][client][http2]")
{
    record                     out;
    std::atomic<std::uint64_t> accepted = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    // close(): the WebSocket's going away (1001), answered (so the client's close() has nothing left to do); abort():
    // the stream's reset, the transport's end (1006).
    const bool abort = GENERATE(false, true);
    CAPTURE(abort);

    auto server = make_server(&sched,
                              routes(
                                  [&](http::server_request& req, http::response_writer& rw) -> task<void>
                                  {
                                      auto conn = co_await ws::accept(req, rw);
                                      if (!conn.has_value()) co_return;
                                      raise(&sched, accepted);

                                      std::string message;
                                      const auto  read = co_await conn->read_message(message);
                                      out.add("server read: " + describe(read.error_or(std::error_code{})));
                                  }));

    http::client client{&sched, prior_knowledge()};

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;
        lines.push_back(co_await fetch(client, url_of(server->local_port(), "http", "/")));

        auto conn = co_await ws::connect(client, url_of(server->local_port()));
        if (!conn.has_value()) co_return std::vector<std::string>{"connect: " + conn.error().message()};

        const bool in = co_await reach(&sched, accepted);
        lines.emplace_back(in ? "accepted" : "not accepted");
        if (abort) server->abort();
        else server->close();

        std::string message;
        const auto  read = co_await conn->read_message(message);
        lines.push_back("read: " + describe(read.error_or(std::error_code{})));
        lines.push_back("close: " + describe(co_await conn->close()));

        co_await server->join();
        co_return lines;
    };

    const auto* const ended = abort ? "1006 peer" : "1001 peer";
    CHECK(sched.run_to_completion(test())
          == std::vector<std::string>{"200 ok",
                                      "accepted",
                                      std::string{"read: "} + ended,
                                      abort ? "close: 1006 peer" : "close: ok"});
    CHECK(out.get()
          == std::vector<std::string>{abort ? "server read: "
                                                  + std::make_error_code(std::errc::operation_canceled).message()
                                            : "server read: 1001 sent"});
}

TEST_CASE("stop ends a WebSocket client's read over HTTP/2, resetting its stream", "[websocket][client][http2][cancel]")
{
    record                     out;
    std::atomic<std::uint64_t> handled = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    std::stop_source stop;

    // the server says it's ready, then requests stop as the client reads on (most likely as it waits).
    auto server = make_server(&sched,
                              routes(
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
                                  }));

    http::client client{&sched, prior_knowledge()};

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;
        lines.push_back(co_await fetch(client, url_of(server->local_port(), "http", "/")));

        auto conn = co_await ws::connect(client, url_of(server->local_port()), {.stop = stop.get_token()});
        if (!conn.has_value()) co_return std::vector<std::string>{"connect: " + conn.error().message()};

        std::string message;
        const auto  ready = co_await conn->read_message(message);
        lines.push_back(ready.has_value() ? message : describe(ready.error()));

        const auto read = co_await conn->read_message(message);
        lines.push_back("read: " + describe(read.error_or(std::error_code{})));
        lines.push_back("write: " + describe(co_await conn->write_message(ws::message_type::text, "x"sv)));

        // the server sees the stream reset (the transport's end, without a close); the connection goes on.
        (void)co_await reach(&sched, handled);
        lines.push_back(co_await fetch(client, url_of(server->local_port(), "http", "/")));
        co_return lines;
    };

    const auto canceled = std::make_error_code(std::errc::operation_canceled).message();
    CHECK(sched.run_to_completion(test())
          == std::vector<std::string>{"200 ok", "ready", "read: " + canceled, "write: " + canceled, "200 ok"});
    CHECK(out.get() == std::vector<std::string>{"read: 1006 peer"});
}

TEST_CASE("a WebSocket client over HTTP/2 outlives its http::client", "[websocket][client][http2]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    auto server =
        make_server(&sched,
                    routes([&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); }));

    // the client goes once the WebSocket's open: its pool drains its HTTP/2 connection (GOAWAY), which stays open for
    // the WebSocket's stream, as for a response's body being read.
    auto client = std::make_unique<http::client>(&sched, prior_knowledge());

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;
        lines.push_back(co_await fetch(*client, url_of(server->local_port(), "http", "/")));

        auto conn = co_await ws::connect(*client, url_of(server->local_port()));
        if (!conn.has_value()) co_return std::vector<std::string>{"connect: " + conn.error().message()};

        client.reset();
        lines.push_back(co_await echoes(*conn, "hello"));
        lines.push_back("close: " + describe(co_await conn->close()));
        co_return lines;
    };

    CHECK(sched.run_to_completion(test()) == std::vector<std::string>{"200 ok", "same", "close: ok"});
    CHECK(out.get() == std::vector<std::string>{"HTTP/2 subprotocol=", "read: 1000 peer reason="});
}

TEST_CASE("a WebSocket client's extended CONNECT, and its stream's end", "[websocket][client][http2]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    script             server{&sched};
    http::client       client{&sched, prior_knowledge()};

    // which side closes first.
    const bool client_closes = GENERATE(true, false);
    CAPTURE(client_closes);

    ws::connect_options options{
        .subprotocols = {"a", "b"},
        .origin       = "https://example.com",
    };
    options.headers.set("Authorization"sv, "Bearer x"sv);
    options.headers.set("Sec-WebSocket-Extensions"sv, "permessage-deflate"sv);

    std::vector<std::pair<std::string, std::string>> request;

    std::atomic<std::uint64_t> client_done = 0; // the client's connection's gone

    const auto serve = [&] -> task<void>
    {
        const bool opened = co_await server.open();
        if (!opened)
        {
            out.add("no extended CONNECT");
            co_return;
        }
        auto& s = server.conn->server;
        request = s.responses[3].headers;

        // an informational response (skipped), then a 2xx with the subprotocol, then a frame.
        const std::vector<http::hpack::field> early{
            {.name = ":status", .value = "103"}
        };
        const std::vector<http::hpack::field> head{
            {               .name = ":status", .value = "200"},
            {.name = "sec-websocket-protocol",   .value = "b"}
        };
        if (auto err = s.s.submit_headers(3, early, false))
        {
            out.add("submit: " + err.message());
            co_return;
        }
        if (auto err = s.s.submit_headers(3, head, false))
        {
            out.add("submit: " + err.message());
            co_return;
        }
        server.send(server_frame(opcode::text, "hi"));

        // the client's message (its first frame: its close may have come with it, if it closes first).
        (void)co_await s.pump([&] { return !server.frames().empty(); });
        if (const auto frames = server.frames(); !frames.empty())
            out.add(std::format("{} {}", frames.front().header.masked ? "masked" : "unmasked", frames.front().text()));

        if (!client_closes) server.send(server_close(ws::close_code::going_away));

        // the client's close, after which it ends its side of the stream at once (before the server's answer, if the
        // client's went first).
        const bool ended  = co_await s.pump([&] { return s.responses[3].done; });
        const auto frames = server.frames();
        out.add(std::format("{} {} {}",
                            ended ? "ended" : "not ended",
                            frames.size(),
                            !frames.empty() && frames.back().header.op == opcode::close ? "close last" : "no close"));

        if (client_closes) server.send(server_close(ws::close_code::normal_closure), true);
        else s.send(3, {}, true);

        // once the client's done with its connection: no reset of the stream (and none before).
        (void)co_await s.pump([] { return true; });
        (void)co_await reach(&sched, client_done);
        (void)co_await s.sync();
        out.add(std::format("resets: {}", std::ranges::count(s.frame_types, http2::frame_type::rst_stream)));
    };

    const auto run = [&] -> task<void>
    {
        {
            auto conn = co_await connect_after_get(client, server.port(), options);
            if (!conn.has_value())
            {
                out.add("connect: " + conn.error().message());
                co_return;
            }
            out.add("subprotocol=" + std::string{conn->subprotocol()});

            std::string message;
            const auto  hi = co_await conn->read_message(message);
            out.add(hi.has_value() ? message : describe(hi.error()));
            (void)co_await conn->write_message(ws::message_type::text, "yo"sv);

            if (client_closes)
            {
                out.add("close: " + describe(co_await conn->close()));
            }
            else
            {
                const auto read = co_await conn->read_message(message);
                out.add("read: " + describe(read.error_or(std::error_code{})));
            }
        }
        raise(&sched, client_done);
    };

    sched.run_to_completion(both(&sched, serve, run));

    const auto has = [&](std::string_view name, std::string_view value)
    { return std::ranges::contains(request, std::pair<std::string, std::string>{name, value}); };
    const auto lacks = [&](std::string_view name)
    { return std::ranges::none_of(request, [&](const auto& f) { return f.first == name; }); };

    CAPTURE(request);
    CHECK(has(":method", "CONNECT"));
    CHECK(has(":protocol", "websocket"));
    CHECK(has(":scheme", "http"));
    CHECK(has(":authority", std::format("127.0.0.1:{}", server.port())));
    CHECK(has(":path", "/ws?x=1"));
    CHECK(has("sec-websocket-version", "13"));
    CHECK(has("sec-websocket-protocol", "a, b"));
    CHECK(has("origin", "https://example.com"));
    CHECK(has("authorization", "Bearer x"));
    for (const auto* const name : {"sec-websocket-key", "sec-websocket-extensions", "upgrade", "connection", "host"})
        CHECK(lacks(name));

    // (the server's lines and the client's interleave: each side's are in order.)
    const auto lines = out.get();
    CAPTURE(lines);
    const auto in_order = [&](const std::vector<std::string>& want)
    {
        auto at = lines.begin();
        for (const auto& w : want)
        {
            at = std::find(at, lines.end(), w);
            if (at == lines.end()) return false;
        }
        return true;
    };
    CHECK(lines.size() == 6);
    CHECK(in_order({"subprotocol=b", "hi"}));
    CHECK(in_order({"masked yo", "ended 2 close last", "resets: 0"}));
    CHECK(in_order({"subprotocol=b", client_closes ? "close: ok" : "read: 1001 peer"}));
}

TEST_CASE("a WebSocket client over HTTP/2 refuses a response that isn't the server's accepting, resetting its stream",
          "[websocket][client][http2][handshake]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    script             server{&sched};
    http::client       client{&sched, prior_knowledge()};

    struct test_case
    {
        std::vector<http::hpack::field> head;
        bool                            end = false; // the head ends the stream
        ws::errc                        err;
    };
    const auto tc = GENERATE(
        test_case{
            .head = {{.name = ":status", .value = "404"}},
            .end  = true,
            .err  = ws::errc::bad_status
    },
        test_case{.head = {{.name = ":status", .value = "302"}, {.name = "location", .value = "/elsewhere"}},
                  .end  = true,
                  .err  = ws::errc::bad_status},
        test_case{.head = {{.name = ":status", .value = "200"}, {.name = "sec-websocket-protocol", .value = "c"}},
                  .err  = ws::errc::bad_subprotocol},
        test_case{.head = {{.name = ":status", .value = "200"},
                           {.name = "sec-websocket-extensions", .value = "permessage-deflate"}},
                  .err  = ws::errc::bad_extension});
    CAPTURE(tc.head[0].value);

    std::string reset;
    const auto  serve = [&] -> task<void>
    {
        if (!co_await server.open()) co_return;
        auto& s = server.conn->server;
        if (auto err = s.s.submit_headers(3, tc.head, tc.end))
        {
            reset = "submit: " + err.message();
            co_return;
        }

        // the client resets the stream (a 2xx's, or one whose request it hasn't ended).
        const bool done = co_await s.pump([&] { return !!s.responses[3].reset; });
        reset           = done ? s.responses[3].reset.message() : "no reset";
    };

    std::error_code err;
    const auto      run = [&] -> task<void>
    {
        auto conn = co_await connect_after_get(client, server.port(), {.subprotocols = {"a"}});
        err       = conn.has_value() ? std::error_code{} : conn.error();
    };

    sched.run_to_completion(both(&sched, serve, run));
    CHECK(err == tc.err);
    CHECK(reset == make_error_code(http2::error_code::cancel).message());
}

TEST_CASE("a WebSocket client's handshake over HTTP/2 times out, and stops", "[websocket][client][http2][cancel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    script             server{&sched};

    // past response_header_timeout, or stopped once the extended CONNECT's come: the server never answers it.
    const bool timeout = GENERATE(true, false);
    CAPTURE(timeout);

    auto options                    = prior_knowledge();
    options.response_header_timeout = 300ms;
    if (!timeout) options.response_header_timeout = 30s;
    http::client client{&sched, options};

    std::stop_source stop;
    std::string      reset;
    const auto       serve = [&] -> task<void>
    {
        if (!co_await server.open()) co_return;
        if (!timeout) stop.request_stop();

        auto&      s    = server.conn->server;
        const bool done = co_await s.pump([&] { return !!s.responses[3].reset; });
        reset           = done ? s.responses[3].reset.message() : "no reset";
    };

    std::error_code err;
    const auto      run = [&] -> task<void>
    {
        auto conn = co_await connect_after_get(client, server.port(), {.stop = stop.get_token()});
        err       = conn.has_value() ? std::error_code{} : conn.error();
    };

    sched.run_to_completion(both(&sched, serve, run));
    CHECK(err == (timeout ? std::errc::timed_out : std::errc::operation_canceled));
    CHECK(reset == make_error_code(http2::error_code::cancel).message());
}

TEST_CASE("a WebSocket client's stream refused by its HTTP/2 server, or asked for HTTP/1.1, goes over HTTP/1.1 instead",
          "[websocket][client][http2]")
{
    // REFUSED_STREAM, or HTTP_1_1_REQUIRED (RFC 9113, section 7) as a reset, or as a GOAWAY whose last stream is the
    // GET's.
    const auto how = GENERATE("refused"sv, "http11"sv, "goaway"sv);
    CAPTURE(how);

    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    script             server{&sched};
    http::client       client{&sched, prior_knowledge()};

    const auto serve = [&] -> task<void>
    {
        if (!co_await server.open()) co_return;
        auto& s = server.conn->server;

        // not processed (or to be sent over HTTP/1.1): the client may open it elsewhere.
        if (how == "refused") s.s.reset(3, http2::error_code::refused_stream);
        else if (how == "http11") s.s.reset(3, http2::error_code::http_1_1_required);
        else s.s.goaway(1, http2::error_code::http_1_1_required);
        (void)co_await s.sync();

        auto second = co_await server.listener.accept();
        if (!second.has_value()) co_return;

        raw_peer   peer{*second, *second};
        const auto head = co_await peer.read_head();
        if (!head.has_value()) co_return;
        out.add(head->substr(0, head->find("\r\n")));

        constexpr auto name = "\r\nSec-WebSocket-Key: "sv;
        const auto     from = head->find(name) + name.size();
        (void)co_await peer.send(
            std::format("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                        "Connection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
                        ws::detail::accept_key(head->substr(from, head->find("\r\n", from) - from))));

        auto f = co_await peer.read_frame();
        if (f.has_value()) out.add(f->header.op == opcode::close ? "close" : "not a close");
        (void)co_await peer.send(server_close(ws::close_code::normal_closure));
        second->shutdown_write();
    };

    const auto run = [&] -> task<void>
    {
        auto conn = co_await connect_after_get(client, server.port());
        out.add(conn.has_value() ? "connected" : "connect: " + conn.error().message());
        if (conn.has_value()) out.add("close: " + describe(co_await conn->close()));
    };

    sched.run_to_completion(both(&sched, serve, run));
    CHECK(out.get() == std::vector<std::string>{"GET /ws?x=1 HTTP/1.1", "connected", "close", "close: ok"});
}

TEST_CASE(
    "a WebSocket client's stream refused, or asked for HTTP/1.1, once its server has answered it isn't opened again",
    "[websocket][client][http2]")
{
    const auto code = GENERATE(http2::error_code::refused_stream, http2::error_code::http_1_1_required);
    CAPTURE(code);

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    script             server{&sched};
    http::client       client{&sched, prior_knowledge()};

    // an interim response says the server processed it: REFUSED_STREAM after that can't mean it never saw it, and
    // HTTP_1_1_REQUIRED after it isn't taken as asking before anything's been done.
    std::atomic<std::uint64_t> connections = 1;
    std::error_code            submitted; // the 103's submitting, if it failed
    const auto                 serve = [&] -> task<void>
    {
        if (!co_await server.open()) co_return;
        auto&                                 s = server.conn->server;
        const std::vector<http::hpack::field> early{
            {.name = ":status", .value = "103"}
        };
        if (auto err = s.s.submit_headers(3, early, false))
        {
            submitted = err;
            co_return;
        }
        (void)co_await s.sync();
        s.s.reset(3, code);
        (void)co_await s.sync();
    };

    std::error_code err;
    const auto      run = [&] -> task<void>
    {
        auto conn = co_await connect_after_get(client, server.port());
        err       = conn.has_value() ? std::error_code{} : conn.error();
        connections.store(client.stats().open);
    };

    sched.run_to_completion(both(&sched, serve, run));
    CHECK(err == (code == http2::error_code::refused_stream ? http2::error_code::cancel : code));
    CHECK_FALSE(submitted);
    CHECK(connections.load() == 1);
}

TEST_CASE("a WebSocket client over HTTP/2 finds its connection's end, and its stream's, as the transport's",
          "[websocket][client][http2]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    script             server{&sched};
    http::client       client{&sched, prior_knowledge()};

    // the connection ends (the server closes it), the stream's reset (with HTTP_1_1_REQUIRED too, too late to go over
    // HTTP/1.1), or the stream ends without a close.
    const auto how = GENERATE("connection"sv, "reset"sv, "http11"sv, "end"sv);
    CAPTURE(how);

    std::atomic<std::uint64_t> reading = 0;

    const auto serve = [&] -> task<void>
    {
        if (!co_await server.open()) co_return;
        auto& s = server.conn->server;
        s.respond(3, "200", {}, false);
        (void)co_await s.sync();
        (void)co_await reach(&sched, reading);

        if (how == "connection") server.conn->socket.shutdown_write();
        else if (how == "reset") s.s.reset(3, http2::error_code::cancel);
        else if (how == "http11") s.s.reset(3, http2::error_code::http_1_1_required);
        else s.send(3, {}, true);
        (void)co_await s.pump([] { return false; }, 300ms);
    };

    std::string read;
    const auto  run = [&] -> task<void>
    {
        auto conn = co_await connect_after_get(client, server.port());
        if (!conn.has_value())
        {
            read = "connect: " + conn.error().message();
            co_return;
        }

        raise(&sched, reading);
        std::string message;
        const auto  got = co_await conn->read_message(message);
        read            = describe(got.error_or(std::error_code{}));
    };

    sched.run_to_completion(both(&sched, serve, run));
    CHECK(read == "1006 peer");
}

TEST_CASE("a WebSocket client's stream isn't reset for being quiet past the client's io_timeout",
          "[websocket][client][http2]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // the server says nothing for a while, longer than the client's io_timeout, as a read waits.
    auto server = make_server(&sched,
                              routes(
                                  [&](http::server_request& req, http::response_writer& rw) -> task<void>
                                  {
                                      auto conn = co_await ws::accept(req, rw);
                                      if (!conn.has_value()) co_return;

                                      (void)co_await sched.sleep_until(net::io::clock::now() + 600ms);
                                      (void)co_await conn->write_message(ws::message_type::text, "late"sv);

                                      std::string message;
                                      const auto  read = co_await conn->read_message(message);
                                      out.add("read: " + describe(read.error_or(std::error_code{})));
                                  }));

    auto options       = prior_knowledge();
    options.io_timeout = 200ms;
    http::client client{&sched, options};

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;
        lines.push_back(co_await fetch(client, url_of(server->local_port(), "http", "/")));

        auto conn = co_await ws::connect(client, url_of(server->local_port()));
        if (!conn.has_value()) co_return std::vector<std::string>{"connect: " + conn.error().message()};

        std::string message;
        const auto  read = co_await conn->read_message(message);
        lines.push_back(read.has_value() ? message : describe(read.error()));
        lines.push_back("close: " + describe(co_await conn->close()));
        co_return lines;
    };

    CHECK(sched.run_to_completion(test()) == std::vector<std::string>{"200 ok", "late", "close: ok"});
    CHECK(out.get() == std::vector<std::string>{"read: 1000 peer"});
}

TEST_CASE("a WebSocket client stopped before it opens its stream sends nothing, and the connection goes on",
          "[websocket][client][http2][cancel]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    auto server =
        make_server(&sched,
                    routes([&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); }));

    http::client client{&sched, prior_knowledge()};

    std::stop_source stop;
    stop.request_stop();

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;
        lines.push_back(co_await fetch(client, url_of(server->local_port(), "http", "/")));

        auto conn = co_await ws::connect(client, url_of(server->local_port()), {.stop = stop.get_token()});
        lines.push_back(conn.has_value() ? "connected" : conn.error().message());

        lines.push_back(co_await fetch(client, url_of(server->local_port(), "http", "/")));
        lines.push_back(std::format("open={}", client.stats().open));
        co_return lines;
    };

    CHECK(sched.run_to_completion(test())
          == std::vector<std::string>{"200 ok",
                                      std::make_error_code(std::errc::operation_canceled).message(),
                                      "200 ok",
                                      "open=1"});
    CHECK(out.get().empty());
}

TEST_CASE("a WebSocket client's stream holds 256 KiB it hasn't read, and its large writes wait for the server's window",
          "[websocket][client][http2][flow]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    script             server{&sched};
    // (the client's stream window the least, so the stream's is the tunnel's own.)
    auto options                = prior_knowledge();
    options.http2_stream_window = 65'535;
    http::client client{&sched, options};

    // pattern is size bytes that differ from one to the next, so a piece in the wrong place shows.
    const auto pattern = [](std::size_t size)
    {
        std::string text(size, '\0');
        for (std::size_t i = 0; i < size; ++i) text[i] = static_cast<char>('a' + (i % 23));
        return text;
    };
    // more than the stream's window, to the client; and much more than the server's stream window, from it.
    const auto in       = pattern(300'000);
    const auto out_text = pattern(std::size_t{1} << 20);

    std::atomic<std::uint64_t> held     = 0; // the server has sent what the client's window allows (the client reads)
    std::atomic<std::uint64_t> released = 0; // the server's begun to give its window back (the client's write goes on)
    std::atomic<std::uint64_t> client_done = 0; // the client's connection's gone

    const auto serve = [&] -> task<void>
    {
        if (!co_await server.open())
        {
            out.add("no extended CONNECT");
            co_return;
        }
        auto& s        = server.conn->server;
        s.hold_windows = true;

        const std::vector<http::hpack::field> head{
            {.name = ":status", .value = "200"}
        };
        if (auto err = s.s.submit_headers(3, head, false))
        {
            out.add("submit: " + err.message());
            co_return;
        }

        // the client widens the stream's window as it opens the tunnel.
        const bool widened = co_await s.pump([&] { return s.s.send_window(3) == 262'144; });
        out.add(std::format("widened {}", widened));

        // the server sends what the window allows, which the client holds unread.
        server.send(test::websocket::frame({.fin = true, .op = opcode::binary}, test::websocket::bytes(in)));
        (void)co_await s.pump([] { return true; });
        out.add(std::format("server's window {}", s.s.send_window(3)));
        raise(&sched, held);

        // the client's write sends what the server's window allows, then waits for it to grow.
        (void)co_await s.pump([&] { return s.responses[3].body.size() >= 65'535; });
        out.add(std::format("client sent {}", s.responses[3].body.size()));
        raise(&sched, released);
        s.hold_windows = false;
        s.s.consume(3, s.responses[3].body.size());

        // (the frame's header: 14 bytes, masked with a 64-bit length.)
        const auto whole = out_text.size() + 14;
        (void)co_await s.pump([&] { return s.responses[3].body.size() >= whole; });
        const auto frames = server.frames();
        out.add(std::format("client's message {}",
                            frames.size() == 1 && frames.front().header.masked && frames.front().text() == out_text
                                ? "same"
                                : "differs"));

        server.send(server_close(ws::close_code::normal_closure), true);
        (void)co_await s.pump([&] { return s.responses[3].done; });
        (void)co_await reach(&sched, client_done);
    };

    const auto run = [&] -> task<void>
    {
        {
            auto conn = co_await connect_after_get(client, server.port());
            if (!conn.has_value())
            {
                out.add("connect: " + conn.error().message());
                co_return;
            }

            (void)co_await reach(&sched, held);
            std::string message;
            const auto  type = co_await conn->read_message(message);
            out.add(type.has_value() ? std::format("read {}", message == in ? "same" : "differs")
                                     : "read: " + describe(type.error()));

            // larger than the tunnel's buffer: masked a buffer's worth at a time, as the server's window allows.
            const auto wrote = co_await conn->write_message(ws::message_type::binary, out_text);
            out.add(std::format("wrote {}, after the window grew: {}", describe(wrote), released.load() == 1));

            const auto closing = co_await conn->read_message(message);
            out.add("then: " + describe(closing.error_or(std::error_code{})));
        }
        raise(&sched, client_done);
    };

    sched.run_to_completion(both(&sched, serve, run));

    const auto lines = out.get();
    CAPTURE(lines);
    CHECK(std::ranges::contains(lines, "widened true"));
    CHECK(std::ranges::contains(lines, "server's window 0"));
    CHECK(std::ranges::contains(lines, "read same"));
    CHECK(std::ranges::contains(lines, "client sent 65535"));
    CHECK(std::ranges::contains(lines, "wrote ok, after the window grew: true"));
    CHECK(std::ranges::contains(lines, "client's message same"));
    CHECK(std::ranges::contains(lines, "then: 1000 peer"));
}

TEST_CASE("a WebSocket client over HTTP/2 widens its stream's window to http2_window",
          "[websocket][client][http2][sizes]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};
    script             server{&sched};

    struct test_case
    {
        std::string_view name;
        std::uint32_t    window        = 0;      // the WebSocket's
        std::uint32_t    stream_window = 65'535; // the client's
        std::int64_t     want          = 0;      // the stream's window, as the server sees it
    };

    const auto tc = GENERATE(test_case{.name = "the default", .window = 262'144, .want = 262'144},
                             test_case{.name = "1 MiB", .window = 1U << 20, .want = 1 << 20},
                             test_case{.name = "the initial window: no wider", .window = 65'535, .want = 65'535},
                             test_case{.name          = "less than the client's stream window: no wider",
                                       .window        = 262'144,
                                       .stream_window = 1U << 20,
                                       .want          = 1 << 20});
    CAPTURE(tc.name);

    auto options                = prior_knowledge();
    options.http2_stream_window = tc.stream_window;
    http::client client{&sched, options};

    std::atomic<std::uint64_t> client_done = 0; // the client's WebSocket's gone

    const auto serve = [&] -> task<void>
    {
        if (!co_await server.open())
        {
            out.add("no extended CONNECT");
            co_return;
        }
        auto& s = server.conn->server;

        const std::vector<http::hpack::field> head{
            {.name = ":status", .value = "200"}
        };
        if (auto err = s.s.submit_headers(3, head, false))
        {
            out.add("submit: " + err.message());
            co_return;
        }

        // the client's close comes after the window's WINDOW_UPDATE, if any, which it sent as it opened the tunnel.
        (void)co_await s.pump([&] { return !server.frames().empty(); });
        out.add(std::format("window {}", s.s.send_window(3)));

        server.send(server_close(ws::close_code::normal_closure), true);
        (void)co_await s.pump([&] { return s.responses[3].done; });
        (void)co_await reach(&sched, client_done);
    };

    const auto run = [&] -> task<void>
    {
        {
            ws::connect_options ws_options;
            ws_options.connection.http2_window = tc.window;
            auto conn                          = co_await connect_after_get(client, server.port(), ws_options);
            if (!conn.has_value())
            {
                out.add("connect: " + conn.error().message());
                co_return;
            }
            out.add("close: " + describe(co_await conn->close()));
        }
        raise(&sched, client_done);
    };

    sched.run_to_completion(both(&sched, serve, run));

    const auto lines = out.get();
    CAPTURE(lines);
    CHECK(std::ranges::contains(lines, std::format("window {}", tc.want)));
    CHECK(std::ranges::contains(lines, "close: ok"));
}

TEST_CASE("a WebSocket client over HTTP/2 echoes messages whatever its buffers' size",
          "[websocket][client][http2][sizes]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    const std::size_t size = GENERATE(1'024, 65'536);
    CAPTURE(size);

    auto server =
        make_server(&sched,
                    routes([&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); }));
    http::client client{&sched, prior_knowledge()};

    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;

        ws::connect_options options;
        options.connection.http2_buffer_size = size;
        auto conn                            = co_await connect_after_get(client, server->local_port(), options);
        if (!conn.has_value()) co_return std::vector<std::string>{"connect: " + conn.error().message()};

        // messages shorter and longer than the buffers, and a frame's header across their ends.
        for (const std::size_t length : {0, 1'000, 1'020, 1'024, 5'000, 70'000, 300'000})
        {
            std::string data(length, '\0');
            for (std::size_t i = 0; i < length; ++i) data[i] = static_cast<char>('a' + ((i + length) % 26));
            lines.push_back(std::format("{} {}", length, co_await echoes(*conn, data)));
        }

        lines.push_back("close: " + describe(co_await conn->close()));
        co_return lines;
    };

    CHECK(sched.run_to_completion(test())
          == std::vector<std::string>{"0 same",
                                      "1000 same",
                                      "1020 same",
                                      "1024 same",
                                      "5000 same",
                                      "70000 same",
                                      "300000 same",
                                      "close: ok"});
    CHECK(std::ranges::contains(out.get(), "HTTP/2 subprotocol="));
}
