// Accepting WebSockets (websocket::accept() and connection), against a scripted client (test/websocket/raw_peer.hpp):
// the handshake and its refusals, messages read and written whole and streamed, control frames, the close handshake,
// failures, how the connection ends, the idle timeout's pings, stopping, and the server's close().
#include "http/server.hpp"

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
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/timeout.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "tcp.hpp"
#include "websocket/frame.hpp"
#include "websocket/frames.hpp"
#include "websocket/raw_peer.hpp"
#include "websocket/websocket.hpp"

namespace http = net::http;
namespace ws   = net::websocket;

namespace
{

using namespace std::chrono_literals;
using namespace std::string_view_literals;

using net::coro::task;
using test::websocket::client_frame;
using test::websocket::close_frame;
using test::websocket::join;
using test::websocket::raw_peer;
using test::websocket::received_frame;
using ws::detail::opcode;

constexpr std::string_view accept_key = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

// record is what handlers report, for the test to check once they've returned (and the connection's ended).
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

std::string_view op_name(opcode op)
{
    switch (op)
    {
    case opcode::continuation: return "cont";
    case opcode::text: return "text";
    case opcode::binary: return "binary";
    case opcode::close: return "close";
    case opcode::ping: return "ping";
    case opcode::pong: return "pong";
    }
    return "reserved";
}

// summary is a frame as text: its type ("+" if it isn't its message's last), and its payload (a close's code and
// reason); "!" first if it's masked or has RSV bits set, which a server's never are.
std::string summary(const received_frame& f)
{
    std::string out = f.header.masked || f.header.rsv != 0 ? "!" : "";
    out += op_name(f.header.op);
    if (!f.header.fin) out += '+';
    out += ':';

    if (f.header.op == opcode::close)
    {
        const auto close = f.close();
        out += std::to_string(static_cast<int>(close.code));
        if (!close.reason.empty()) out += " " + std::string{close.reason};
        return out;
    }
    return out + f.text();
}

std::vector<std::string> summaries(const std::vector<received_frame>& frames)
{
    std::vector<std::string> out;
    out.reserve(frames.size());
    for (const auto& f : frames) out.push_back(summary(f));
    return out;
}

// head_of is the response's head, up to the empty line.
std::string_view head_of(std::string_view received)
{
    const auto end = received.find("\r\n\r\n"sv);
    return end == std::string_view::npos ? received : received.substr(0, end + 4);
}

task<std::expected<net::tcp_socket, std::error_code>> connect(net::io::scheduler* sched, std::uint16_t port)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    if (conn.has_value()) conn->set_read_timeout(5s);
    co_return conn;
}

std::unique_ptr<http::server> make_server(net::io::scheduler* sched, http::router router, http::server_config cfg = {})
{
    cfg.host = "127.0.0.1";
    cfg.port = "0";

    auto server = std::make_unique<http::server>(sched, std::move(router), cfg);
    REQUIRE(sched->spawn(server->serve()));
    return server;
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

// echo accepts a WebSocket and echoes its messages whole, until a read or write fails, which it records.
// NOTE: options is taken by value, as handlers return echo()'s task rather than await it: a reference would outlive the
//       options given (or the default's temporary) when the task runs.
task<void>
echo(const http::server_request& req, http::response_writer& rw, record& out, ws::accept_options options = {})
{
    auto conn = co_await ws::accept(req, rw, options);
    if (!conn.has_value())
    {
        out.add("refused: " + conn.error().message());
        co_return;
    }

    std::string message;
    while (true)
    {
        const auto type = co_await conn->read_message(message);
        if (!type.has_value())
        {
            out.add(std::format("read: {} reason={}", describe(type.error()), conn->close_reason()));

            // every operation after a failure returns it again.
            const auto wrote = co_await conn->write_message(ws::message_type::text, "late"sv);
            out.add("then write: " + describe(wrote));
            out.add("then close: " + describe(co_await conn->close()));
            co_return;
        }

        if (auto err = co_await conn->write_message(*type, message))
        {
            out.add("write: " + describe(err));
            co_return;
        }
    }
}

// read_full reads into buf until it's full or the message ends, returning how much it read and how it ended.
task<net::io::result> read_full(net::io::reader& message, std::span<std::byte> buf)
{
    std::size_t total = 0;
    while (total < buf.size())
    {
        auto res = co_await message.read(buf.subspan(total));
        total += res.count;
        if (res.err) co_return net::io::result{.count = total, .err = res.err};
    }
    co_return net::io::result{.count = total};
}

std::string as_text(std::span<const std::byte> bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// run_peer connects, sends the handshake (with frames along with it, if early), then the frames (piece bytes per write,
// or at once), and reads what comes back until the connection ends, returning the response's head, the frames and how
// it ended.
struct peer_result
{
    std::string                 head;
    std::vector<received_frame> frames;
    std::error_code             err;
};

task<peer_result> run_peer(net::io::scheduler*    sched,
                           std::uint16_t          port,
                           std::string            request,
                           std::vector<std::byte> frames,
                           bool                   early = false,
                           std::size_t            piece = 0)
{
    peer_result out;
    auto        conn = co_await connect(sched, port);
    if (!conn.has_value())
    {
        out.err = conn.error();
        co_return out;
    }

    raw_peer peer{*conn, *conn};
    if (early) (void)co_await peer.send(join(test::websocket::bytes(request), frames));
    else (void)co_await peer.send(request);

    auto head = co_await peer.read_head();
    if (!head.has_value())
    {
        out.err = head.error();
        co_return out;
    }
    out.head = std::move(*head);

    // sent alongside the reads (a task of its own), as the server's echoes may wait for the peer to read before it
    // reads on.
    std::atomic<std::uint64_t> sent = early ? 1 : 0;
    if (!early)
    {
        const auto sender = [](raw_peer*                   peer,
                               std::span<const std::byte>  frames,
                               std::size_t                 piece,
                               net::io::scheduler*         sched,
                               std::atomic<std::uint64_t>* sent) -> task<void>
        {
            (void)co_await peer->send(frames, piece);
            sent->store(1);
            sched->notify_all(*sent);
        };
        if (!sched->spawn(sender(&peer, frames, piece, sched, &sent))) sent.store(1);
    }

    auto [received, err] = co_await peer.read_until_end();
    out.frames           = std::move(received);
    out.err              = err;

    // the sender's done with the connection (its last write may fail, once the server has closed).
    while (sent.load() == 0) (void)co_await sched->wait_on(sent, 0);
    co_return out;
}

}

TEST_CASE("a WebSocket handler echoes messages, answering pings between fragments", "[websocket][server]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // whether the handler waits before it accepts, so the connection's watched meanwhile (unless the client sent
    // frames along with its request, which are buffered: then there's no need), and the switch ends the watch.
    const bool waits = GENERATE(false, true);

    // whether the client sends its frames along with its request, which the connection keeps; and whether it sends
    // them a byte at a time.
    const bool        early = GENERATE(false, true);
    const std::size_t piece = early ? 0 : GENERATE(0, 1);
    CAPTURE(waits, early, piece);

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   if (waits) (void)co_await sched.sleep_until(net::io::clock::now() + 50ms);
                   co_await echo(req, rw, out);
               });

    auto server = make_server(&sched, std::move(router));

    const std::array<std::byte, 5> binary{std::byte{0x00},
                                          std::byte{0x01},
                                          std::byte{0x41},
                                          std::byte{0x7f},
                                          std::byte{0xff}};
    const auto                     frames = join(client_frame(opcode::text, "hello"sv),
                                                 client_frame(opcode::binary, binary),
                                                 client_frame(opcode::text, "frag"sv, false),
                                                 client_frame(opcode::ping, "p1"sv),
                                                 client_frame(opcode::continuation, "men"sv, false),
                                                 client_frame(opcode::ping, "p2"sv),
                                                 client_frame(opcode::pong, "unsolicited"sv),
                                                 client_frame(opcode::continuation, "ted"sv),
                                                 client_frame(opcode::text, ""sv),
                                                 close_frame(ws::close_code::normal_closure, "bye"));

    const auto result = sched.run_to_completion(
        run_peer(&sched, server->local_port(), test::websocket::handshake(), frames, early, piece));
    CAPTURE(result.head);

    CHECK(result.head.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    REQUIRE(result.frames.size() == 7);
    CHECK(summaries(result.frames)
          == std::vector<std::string>{"text:hello",
                                      "binary:" + as_text(binary),
                                      "pong:p1",
                                      "pong:p2",
                                      "text:fragmented",
                                      "text:",
                                      "close:1000"});

    // the server closed TCP after the close handshake, having nothing left to send.
    CHECK(result.err == net::io::errc::end_of_stream);
    // and, the close handshake being done, closing finds nothing to do.
    CHECK(out.get()
          == std::vector<std::string>{"read: 1000 peer reason=bye", "then write: 1000 peer", "then close: ok"});
}

TEST_CASE("a WebSocket handler echoes messages of each length form", "[websocket][server]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // around the 7-bit, 16-bit and 64-bit lengths' edges, larger than the connection's buffers, and the limit.
    const std::size_t size = GENERATE(0, 1, 125, 126, 127, 65'535, 65'536, 65'537, 300'000, std::size_t{1} << 20);

    // in one frame, or in three (the limit applies to the message).
    const bool fragmented = GENERATE(false, true);
    CAPTURE(size, fragmented);

    http::router router;
    router.GET("/ws", [&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); });
    auto server = make_server(&sched, std::move(router));

    std::string text(size, '\0');
    for (std::size_t i = 0; i < size; ++i) text[i] = static_cast<char>('a' + (i % 26));
    std::vector<std::byte> binary(size);
    for (std::size_t i = 0; i < size; ++i) binary[i] = static_cast<std::byte>(i * 7);

    std::vector<std::byte> frames;
    if (fragmented)
    {
        const auto third = size / 3;
        frames           = join(client_frame(opcode::text, std::string_view{text}.substr(0, third), false),
                                client_frame(opcode::continuation, std::string_view{text}.substr(third, third), false),
                                client_frame(opcode::continuation, std::string_view{text}.substr(2 * third)),
                                client_frame(opcode::binary, std::span{binary}.first(third), false),
                                client_frame(opcode::continuation, std::span{binary}.subspan(third)));
    }
    else
    {
        frames = join(client_frame(opcode::text, text), client_frame(opcode::binary, binary));
    }
    frames = join(frames, close_frame(ws::close_code::normal_closure));

    const auto result =
        sched.run_to_completion(run_peer(&sched, server->local_port(), test::websocket::handshake(), frames));
    CAPTURE(result.err.message(), out.get());

    REQUIRE(result.frames.size() == 3);
    CHECK(result.frames[0].header.op == opcode::text);
    CHECK(result.frames[0].header.fin);
    CHECK(result.frames[0].text() == text);
    CHECK(result.frames[1].header.op == opcode::binary);
    CHECK(result.frames[1].payload == binary);
    CHECK(summary(result.frames[2]) == "close:1000");
    CHECK(result.err == net::io::errc::end_of_stream);
}

TEST_CASE("a WebSocket handler streams messages in and out", "[websocket][server][stream]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto accepted = co_await ws::accept(req, rw);
                   if (!accepted.has_value()) co_return;

                   // moved, and moved back (into the one moved from).
                   ws::connection moved{std::move(*accepted)};
                   auto           conn = std::optional<ws::connection>{std::move(moved)};
                   moved               = std::move(*conn);
                   *conn               = std::move(moved);

                   // the first message, read 7 bytes at a time, each written back as a frame of a message of its own.
                   const auto type = co_await conn->next_message();
                   if (!type.has_value()) co_return;

                   auto writer = co_await conn->begin_message(*type);
                   if (!writer.has_value()) co_return;

                   std::array<std::byte, 7> piece{};
                   while (true)
                   {
                       const auto read = co_await read_full(conn->message(), piece);
                       if (read.count > 0) (void)co_await writer->write(std::span{piece}.first(read.count));
                       if (read.err)
                       {
                           out.add("first: " + describe(read.err));
                           break;
                       }
                   }
                   out.add("end: " + describe(co_await writer->end_message()));

                   // the message's end, again.
                   const auto again = co_await conn->message().read(piece);
                   out.add(std::format("again: {} {}", again.count, describe(again.err)));

                   // the second, of which only 3 bytes are read: next_message() skips the rest.
                   (void)co_await conn->next_message();
                   const auto some = co_await read_full(conn->message(), std::span{piece}.first(3));
                   (void)co_await conn->write_message(ws::message_type::text,
                                                      "B:" + as_text(std::span{piece}.first(some.count)));

                   // the third, streamed whole.
                   (void)co_await conn->next_message();
                   const auto third = co_await read_full(conn->message(), piece);
                   (void)co_await conn->write_message(ws::message_type::text, std::span{piece}.first(third.count));

                   // the fourth, of which 2 bytes are read: read_message() skips the rest.
                   (void)co_await conn->next_message();
                   const auto fourth = co_await read_full(conn->message(), std::span{piece}.first(2));
                   (void)co_await conn->write_message(ws::message_type::text,
                                                      "D:" + as_text(std::span{piece}.first(fourth.count)));

                   std::string message;
                   (void)co_await conn->read_message(message);
                   (void)co_await conn->write_message(ws::message_type::binary, message);

                   const auto closed = co_await conn->read_message(message);
                   out.add("closed: " + describe(closed.error_or(std::error_code{})));
               });
    auto server = make_server(&sched, std::move(router));

    const auto frames = join(client_frame(opcode::text, "abcdefghij"sv, false),
                             client_frame(opcode::ping, "x"sv),
                             client_frame(opcode::continuation, "klmnopqrst"sv),
                             client_frame(opcode::text, "0123456789"sv),
                             client_frame(opcode::text, "third"sv),
                             client_frame(opcode::binary, "par"sv, false),
                             client_frame(opcode::continuation, "tial"sv),
                             client_frame(opcode::text, "whole"sv),
                             close_frame(ws::close_code::going_away));

    const auto result =
        sched.run_to_completion(run_peer(&sched, server->local_port(), test::websocket::handshake(), frames));

    // the frames as written: the pong goes between them, as the read that came to the ping sent it.
    CHECK(summaries(result.frames)
          == std::vector<std::string>{"text+:abcdefg",
                                      "pong:x",
                                      "cont+:hijklmn",
                                      "cont+:opqrst",
                                      "cont:",
                                      "text:B:012",
                                      "text:third",
                                      "text:D:pa",
                                      "binary:whole",
                                      "close:1001"});
    CHECK(result.err == net::io::errc::end_of_stream);
    CHECK(out.get()
          == std::vector<std::string>{"first: " + make_error_code(net::io::errc::end_of_stream).message(),
                                      "end: ok",
                                      "again: 0 " + make_error_code(net::io::errc::end_of_stream).message(),
                                      "closed: 1001 peer"});
}

TEST_CASE("a WebSocket's 101 says what the handshake agreed", "[websocket][server][handshake]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // whether the client offers subprotocols, of which the server chooses its own first.
    const bool offers = GENERATE(false, true);
    CAPTURE(offers);

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   rw.headers().set("Set-Cookie"sv, "a=b"sv);

                   auto conn = co_await ws::accept(req,
                                                   rw,
                                                   {
                                                       .subprotocols = {"chat.v2", "chat"}
                   });
                   if (!conn.has_value()) co_return;
                   out.add(std::format("subprotocol={}", conn->subprotocol()));

                   std::string message;
                   (void)co_await conn->read_message(message);
               });
    auto server = make_server(&sched, std::move(router));

    const auto request =
        test::websocket::handshake("/ws",
                                   offers ? "Sec-WebSocket-Protocol: chat, x\r\nSec-WebSocket-Protocol: chat.v2\r\n"
                                            "Sec-WebSocket-Extensions: permessage-deflate\r\n"sv
                                          : "Sec-WebSocket-Extensions: permessage-deflate\r\n"sv);
    const auto result = sched.run_to_completion(
        run_peer(&sched, server->local_port(), request, close_frame(ws::close_code::normal_closure)));
    CAPTURE(result.head);

    CHECK(result.head.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(result.head.contains("\r\nUpgrade: websocket\r\n"));
    CHECK(result.head.contains("\r\nConnection: Upgrade\r\n"));
    CHECK(result.head.contains(std::format("\r\nSec-WebSocket-Accept: {}\r\n", accept_key)));
    CHECK(result.head.contains("\r\nSet-Cookie: a=b\r\n"));
    CHECK(result.head.contains("\r\nSec-WebSocket-Protocol: chat.v2\r\n") == offers);
    CHECK_FALSE(result.head.contains("Sec-WebSocket-Extensions"));
    CHECK_FALSE(result.head.contains("Content-Length"));
    CHECK(summaries(result.frames) == std::vector<std::string>{"close:1000"});
    CHECK(out.get() == std::vector<std::string>{offers ? "subprotocol=chat.v2" : "subprotocol="});
}

TEST_CASE("a refused WebSocket handshake is answered, and the connection serves the next request",
          "[websocket][server][handshake]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    struct test_case
    {
        std::string_view              name;
        std::string                   request;
        std::string_view              status;
        std::vector<std::string_view> fields{}; // the response must have
        ws::errc                      why;
    };

    const auto tc = GENERATE(
        test_case{
            .name    = "not an upgrade",
            .request = "GET /ws HTTP/1.1\r\nHost: x\r\n\r\n",
            .status  = "HTTP/1.1 400 Bad Request\r\n",
            .why     = ws::errc::not_websocket,
    },
        test_case{
            .name    = "not a GET",
            .request = "POST /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       "Content-Length: 0\r\n\r\n",
            .status  = "HTTP/1.1 400 Bad Request\r\n",
            .why     = ws::errc::not_websocket,
        },
        test_case{
            .name    = "a bad key",
            .request = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: c2hvcnQ=\r\n\r\n",
            .status  = "HTTP/1.1 400 Bad Request\r\n",
            .why     = ws::errc::bad_key,
        },
        test_case{
            .name    = "another version",
            .request = "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Version: 8\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
            .status  = "HTTP/1.1 426 Upgrade Required\r\n",
            .fields  = {"\r\nSec-WebSocket-Version: 13\r\n",
                        "\r\nUpgrade: websocket\r\n",
                        "\r\nConnection: Upgrade\r\n"},
            .why     = ws::errc::bad_version,
        },
        test_case{
            .name    = "another origin",
            .request = test::websocket::handshake("/ws", "Origin: http://elsewhere.example\r\n"),
            .status  = "HTTP/1.1 403 Forbidden\r\n",
            .why     = ws::errc::origin_refused,
        });
    CAPTURE(tc.name);

    http::router router;
    const auto   handler = [&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); };
    router.GET("/ws", handler);
    router.POST("/ws", handler);
    router.GET("/plain",
               [](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto* body = co_await rw.send(http::status::OK, 5);
                   (void)co_await net::io::write_all(*body, "plain"sv);
               });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::string>
    {
        auto conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return "connect failed";

        (void)co_await net::io::write_all(*conn,
                                          tc.request + "GET /plain HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        auto read = co_await net::io::read_all(*conn, 1 << 16);
        co_return read.value_or("read failed");
    };

    const auto received = sched.run_to_completion(client());
    CAPTURE(received);

    const auto first = head_of(received);
    CHECK(first.starts_with(tc.status));
    for (const auto field : tc.fields) CHECK(first.contains(field));
    CHECK(received.contains(make_error_code(tc.why).message() + "\nHTTP/1.1 200 OK\r\n"));
    CHECK(received.ends_with("\r\n\r\nplain"));
    CHECK(out.get() == std::vector<std::string>{"refused: " + make_error_code(tc.why).message()});
}

TEST_CASE("a WebSocket fails, telling the peer, when it breaks the protocol", "[websocket][server][failure]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    struct test_case
    {
        std::string_view       name;
        std::vector<std::byte> frames;
        std::string_view       closed; // the close the server sent
        std::string_view       read;   // what the handler's read returned
    };

    const std::array<std::byte, 3> bad_utf8{std::byte{'a'}, std::byte{0xc3}, std::byte{0x28}};
    const std::array<std::byte, 2> split_char{std::byte{0xe2}, std::byte{0x82}};

    const auto tc = GENERATE_REF(
        test_case{
            .name   = "unmasked",
            .frames = client_frame(opcode::text, "hi"sv, true, false),
            .closed = "close:1002",
            .read   = "read: 1002 sent reason=",
        },
        test_case{
            .name   = "RSV bits",
            .frames = test::websocket::frame(
                {.rsv = 4, .op = opcode::text, .masked = true, .mask_key = test::websocket::mask_key},
                test::websocket::bytes("hi")),
            .closed = "close:1002",
            .read   = "read: 1002 sent reason=",
        },
        test_case{
            .name   = "a continuation without a message",
            .frames = client_frame(opcode::continuation, "hi"sv),
            .closed = "close:1002",
            .read   = "read: 1002 sent reason=",
        },
        test_case{
            .name   = "a close of one byte",
            .frames = client_frame(opcode::close, "x"sv),
            .closed = "close:1002",
            .read   = "read: 1002 sent reason=",
        },
        test_case{
            .name   = "text that isn't UTF-8",
            .frames = client_frame(opcode::text, bad_utf8),
            .closed = "close:1007",
            .read   = "read: 1007 sent reason=",
        },
        test_case{
            .name   = "text ending part way through a character, after a ping",
            .frames = join(client_frame(opcode::text, split_char, false),
                           client_frame(opcode::ping, "p"sv),
                           client_frame(opcode::continuation, ""sv)),
            .closed = "close:1007",
            .read   = "read: 1007 sent reason=",
        },
        test_case{
            .name   = "past max_message",
            .frames = join(client_frame(opcode::binary, std::string(600, 'x'), false),
                           client_frame(opcode::continuation, std::string(401, 'x'))),
            .closed = "close:1009",
            .read   = "read: 1009 sent reason=",
        });
    CAPTURE(tc.name);

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw)
               { return echo(req, rw, out, {.connection = {.max_message = 1'000}}); });
    auto server = make_server(&sched, std::move(router));

    const auto result =
        sched.run_to_completion(run_peer(&sched, server->local_port(), test::websocket::handshake(), tc.frames));

    // the close (after a pong, for the ping), then the server closes TCP, as the handler returned.
    auto frames = summaries(result.frames);
    std::erase(frames, "pong:p");
    CHECK(frames == std::vector<std::string>{std::string{tc.closed}});
    CHECK(result.err == net::io::errc::end_of_stream);
    CHECK(out.get()
          == std::vector<std::string>{std::string{tc.read},
                                      "then write: " + std::string{tc.read.substr(6, 9)},
                                      "then close: " + std::string{tc.read.substr(6, 9)}});
}

TEST_CASE("a WebSocket handler closes, and the peer's answer ends it", "[websocket][server][close]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw);
                   if (!conn.has_value()) co_return;

                   (void)co_await conn->write_message(ws::message_type::text, "hi"sv);

                   // what can't be sent isn't.
                   out.add("bad code: " + describe(co_await conn->close(ws::close_code::abnormal_closure)));
                   out.add("long reason: "
                           + describe(co_await conn->close(ws::close_code{4000}, std::string(124, 'r'))));

                   out.add("close: " + describe(co_await conn->close(ws::close_code{4000}, "done")));
                   out.add(std::format("reason={}", conn->close_reason()));

                   // done: closing again finds nothing to do.
                   out.add("again: " + describe(co_await conn->close()));

                   std::string message;
                   const auto  read = co_await conn->read_message(message);
                   out.add("read: " + describe(read.error_or(std::error_code{})));
               });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        // the message, and the server's close.
        for (int i = 0; i < 2; ++i)
        {
            auto f = co_await peer.read_frame();
            if (!f.has_value()) break;
            result.frames.push_back(std::move(*f));
        }

        // a message the server drops, then the answer.
        (void)co_await peer.send(join(client_frame(opcode::text, "dropped"sv),
                                      client_frame(opcode::ping, "dropped too"sv),
                                      close_frame(ws::close_code{4001}, "ok")));

        auto [rest, err] = co_await peer.read_until_end();
        result.frames.insert(result.frames.end(), rest.begin(), rest.end());
        result.err = err;
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(summaries(result.frames) == std::vector<std::string>{"text:hi", "close:4000 done"});
    CHECK(result.err == net::io::errc::end_of_stream);
    const auto invalid = std::make_error_code(std::errc::invalid_argument).message();
    CHECK(out.get()
          == std::vector<std::string>{"bad code: " + invalid,
                                      "long reason: " + invalid,
                                      "close: ok",
                                      "reason=ok",
                                      "again: ok",
                                      "read: 4001 peer"});
}

TEST_CASE("a WebSocket's close waits for the task that's reading to see the peer's", "[websocket][server][close]")
{
    record                     out;
    std::atomic<std::uint64_t> read_once = 0;
    std::atomic<std::uint64_t> finished  = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw);
                   if (!conn.has_value()) co_return;

                   if (!sched.spawn(
                           [](ws::connection*             conn,
                              record*                     out,
                              net::io::scheduler*         sched,
                              std::atomic<std::uint64_t>* read_once,
                              std::atomic<std::uint64_t>* finished) -> task<void>
                           {
                               std::string message;
                               while (true)
                               {
                                   const auto read = co_await conn->read_message(message);
                                   if (!read.has_value())
                                   {
                                       out->add("reader: " + describe(read.error()));
                                       break;
                                   }
                                   out->add("reader: " + message);
                                   read_once->store(1);
                                   sched->notify_all(*read_once);
                               }
                               finished->store(1);
                               sched->notify_all(*finished);
                           }(&*conn, &out, &sched, &read_once, &finished)))
                       out.add("spawn failed");

                   const auto deadline = net::io::clock::now() + 5s;
                   while (read_once.load() == 0)
                   {
                       if (co_await sched.wait_on(read_once, 0, deadline)) break;
                   }

                   // the reader's waiting for the next message by then: one read at a time.
                   (void)co_await sched.sleep_until(net::io::clock::now() + 20ms);
                   std::string other;
                   const auto  busy = co_await conn->read_message(other);
                   out.add("busy: " + describe(busy.error_or(std::error_code{})));

                   out.add("close: " + describe(co_await conn->close()));

                   while (finished.load() == 0)
                   {
                       if (co_await sched.wait_on(finished, 0, deadline)) break;
                   }
               });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(
            join(test::websocket::bytes(test::websocket::handshake()), client_frame(opcode::text, "one"sv)));
        result.head = (co_await peer.read_head()).value_or("");

        auto f = co_await peer.read_frame();
        if (!f.has_value()) co_return result;
        result.frames.push_back(std::move(*f));

        (void)co_await peer.send(close_frame(ws::close_code::normal_closure));
        auto [rest, err] = co_await peer.read_until_end();
        result.frames.insert(result.frames.end(), rest.begin(), rest.end());
        result.err = err;
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(summaries(result.frames) == std::vector<std::string>{"close:1000"});
    CHECK(result.err == net::io::errc::end_of_stream);
    // the reader's end, and close()'s, in either order.
    auto lines = out.get();
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == "reader: one");
    CHECK(lines[1] == "busy: " + std::make_error_code(std::errc::device_or_resource_busy).message());
    std::ranges::sort(lines.begin() + 2, lines.end());
    CHECK(lines[2] == "close: ok");
    CHECK(lines[3] == "reader: 1000 peer");
}

TEST_CASE("a WebSocket's close gives up on a peer that doesn't answer", "[websocket][server][close]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // whether the peer ends its side of the connection, rather than wait.
    const bool hangs_up = GENERATE(false, true);
    CAPTURE(hangs_up);

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw, {.connection = {.close_timeout = 200ms}});
                   if (!conn.has_value()) co_return;

                   const auto start  = net::io::clock::now();
                   const auto closed = co_await conn->close();
                   const auto took   = net::io::clock::now() - start;
                   out.add(std::format("close: {} waited={}", describe(closed), took >= 200ms));
                   out.add("then: " + describe(co_await conn->ping()));
               });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        // messages, but no close.
        (void)co_await peer.send(client_frame(opcode::text, "not a close"sv));
        if (hangs_up) conn->shutdown_write();

        auto [frames, err] = co_await peer.read_until_end();
        result.frames      = std::move(frames);
        result.err         = err;
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(summaries(result.frames) == std::vector<std::string>{"close:1000"});
    CHECK(result.err == net::io::errc::end_of_stream);
    const auto why = hangs_up ? "1006 peer" : std::make_error_code(std::errc::timed_out).message();
    CHECK(out.get() == std::vector<std::string>{std::format("close: {} waited={}", why, !hangs_up), "then: " + why});
}

TEST_CASE("a WebSocket handler that returns without closing aborts the connection", "[websocket][server][close]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw);
                   if (conn.has_value()) (void)co_await conn->write_message(ws::message_type::text, "bye"sv);
               });
    auto server = make_server(&sched, std::move(router));

    const auto result =
        sched.run_to_completion(run_peer(&sched, server->local_port(), test::websocket::handshake(), {}));

    // what was written, but no close.
    CHECK(summaries(result.frames) == std::vector<std::string>{"text:bye"});
    CHECK(result.err == net::io::errc::end_of_stream);
}

TEST_CASE("aborting the server ends a WebSocket's read, canceled", "[websocket][server][cancel]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws", [&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        server->abort();
        auto [frames, err] = co_await peer.read_until_end();
        result.frames      = std::move(frames);
        result.err         = err;

        co_await server->join();
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(result.frames.empty());
    CHECK(result.err == net::io::errc::end_of_stream);

    // through the request's token, which is the WebSocket's stop by default: then the reading side's shut.
    const auto canceled = std::make_error_code(std::errc::operation_canceled).message();
    CHECK(out.get()
          == std::vector<std::string>{"read: " + canceled + " reason=",
                                      "then write: " + canceled,
                                      "then close: " + canceled});
}

TEST_CASE("a WebSocket counts against max_connections until its handler returns", "[websocket][server][limits]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws", [&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); });
    router.GET("/plain",
               [](http::server_request& /*req*/, http::response_writer& rw) -> task<void>
               {
                   auto* body = co_await rw.send(http::status::OK, 5);
                   (void)co_await net::io::write_all(*body, "plain"sv);
               });
    auto server = make_server(&sched, std::move(router), {.max_connections = 1});

    const auto client = [&] -> task<std::string>
    {
        auto first = co_await connect(&sched, server->local_port());
        if (!first.has_value()) co_return "connect failed";

        raw_peer peer{*first, *first};
        (void)co_await peer.send(test::websocket::handshake());
        if (!(co_await peer.read_head()).has_value()) co_return "no 101";

        // the second connects (the kernel accepts it), but isn't served while the WebSocket is open.
        auto second = co_await connect(&sched, server->local_port());
        if (!second.has_value()) co_return "connect failed";

        (void)co_await net::io::write_all(*second, "GET /plain HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"sv);
        std::array<char, 64> buf{};
        second->set_read_deadline(net::io::clock::now() + 300ms);
        const auto waited = co_await second->read(buf);
        if (waited.err != std::errc::timed_out) co_return "served while the WebSocket was open";
        second->set_read_deadline(std::nullopt);

        (void)co_await peer.send(close_frame(ws::close_code::normal_closure));
        (void)co_await peer.read_until_end();

        auto read = co_await net::io::read_all(*second, 1 << 16);
        co_return read.value_or("read failed");
    };

    const auto received = sched.run_to_completion(client());
    CAPTURE(received);
    CHECK(received.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(received.ends_with("\r\n\r\nplain"));
}

TEST_CASE("a WebSocket's writers take turns a message at a time", "[websocket][server][write]")
{
    constexpr int tasks    = 4;
    constexpr int messages = 40;
    constexpr int parts    = 10;

    record                     out;
    std::atomic<std::uint64_t> finished = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw);
                   if (!conn.has_value()) co_return;

                   const auto writer = [](ws::connection*             conn,
                                          int                         id,
                                          net::io::scheduler*         sched,
                                          std::atomic<std::uint64_t>* finished) -> task<void>
                   {
                       for (int i = 0; i < messages; ++i)
                       {
                           // large enough that writes wait for the peer to read some.
                           const auto message =
                               std::format("{}-{}:{}", id, i, std::string(20'000, static_cast<char>('a' + id)));
                           if (co_await conn->write_message(ws::message_type::text, message)) break;
                       }
                       finished->fetch_add(1);
                       sched->notify_all(*finished);
                   };
                   for (int id = 0; id < tasks; ++id)
                   {
                       if (!sched.spawn(writer(&*conn, id, &sched, &finished))) out.add("spawn failed");
                   }

                   // a message a frame at a time, alongside, with pings between its frames.
                   auto streamed = co_await conn->begin_message(ws::message_type::binary);
                   if (streamed.has_value())
                   {
                       for (int i = 0; i < parts; ++i)
                       {
                           (void)co_await streamed->write(std::format("part{};", i));
                           (void)co_await conn->ping(std::as_bytes(std::span{"between"sv}));
                       }
                       out.add("end: " + describe(co_await streamed->end_message()));
                   }

                   const auto deadline = net::io::clock::now() + 10s;
                   while (finished.load() < tasks)
                   {
                       if (co_await sched.wait_on(finished, finished.load(), deadline)) break;
                   }
                   out.add("close: " + describe(co_await conn->close()));
               });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        while (true)
        {
            auto f = co_await peer.read_frame();
            if (!f.has_value()) break;
            const bool closing = f->header.op == opcode::close;
            result.frames.push_back(std::move(*f));
            if (closing) break;
        }

        (void)co_await peer.send(close_frame(ws::close_code::normal_closure));
        auto [rest, err] = co_await peer.read_until_end();
        result.err       = err;
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    REQUIRE_FALSE(result.frames.empty());
    CHECK(summary(result.frames.back()) == "close:1000");
    CHECK(result.err == net::io::errc::end_of_stream);
    CHECK(out.get() == std::vector<std::string>{"end: ok", "close: ok"});

    // each task's messages whole and in order, and the streamed message's frames together, but for pings.
    std::array<int, tasks> next{};
    int                    pings = 0;
    std::string            streamed;
    bool                   streaming = false;
    bool                   whole     = true;
    for (const auto& f : result.frames)
    {
        switch (f.header.op)
        {
        case opcode::ping: ++pings; continue;
        case opcode::close: continue;
        case opcode::text:
        {
            whole           = whole && !streaming && f.header.fin;
            const auto text = f.text();
            const auto id   = text[0] - '0';
            whole           = whole && id >= 0 && id < tasks
                           && text == std::format("{}-{}:{}", id, next[id], std::string(20'000, static_cast<char>('a' + id)));
            if (id >= 0 && id < tasks) ++next[id];
            continue;
        }
        case opcode::binary:
            whole     = whole && !streaming && !f.header.fin;
            streaming = true;
            streamed += f.text();
            continue;
        case opcode::continuation:
            whole = whole && streaming;
            streamed += f.text();
            streaming = !f.header.fin;
            continue;
        default: whole = false;
        }
    }

    CHECK(whole);
    CHECK_FALSE(streaming);
    CHECK(pings == parts);
    CHECK(streamed == "part0;part1;part2;part3;part4;part5;part6;part7;part8;part9;");
    CHECK(next == std::array<int, tasks>{messages, messages, messages, messages});
}

TEST_CASE("a WebSocket's control frames go between a streamed message's frames, and whole messages after it",
          "[websocket][server][write]")
{
    record                     out;
    std::atomic<std::uint64_t> finished = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET(
        "/ws",
        [&](http::server_request& req, http::response_writer& rw) -> task<void>
        {
            auto conn = co_await ws::accept(req, rw);
            if (!conn.has_value()) co_return;

            auto streamed = co_await conn->begin_message(ws::message_type::text);
            if (!streamed.has_value()) co_return;
            (void)co_await streamed->write("one"sv);

            // a whole message waits for the streamed one's end; and so does another streamed one.
            (void)sched.spawn(
                [](ws::connection* conn, record* out, net::io::scheduler* sched, std::atomic<std::uint64_t>* finished)
                    -> task<void>
                {
                    out->add("whole: " + describe(co_await conn->write_message(ws::message_type::binary, "whole"sv)));
                    finished->fetch_add(1);
                    sched->notify_all(*finished);
                }(&*conn, &out, &sched, &finished));
            (void)sched.spawn(
                [](ws::connection* conn, record* out, net::io::scheduler* sched, std::atomic<std::uint64_t>* finished)
                    -> task<void>
                {
                    auto second = co_await conn->begin_message(ws::message_type::text);
                    if (second.has_value())
                    {
                        (void)co_await second->write("second"sv);
                        out->add("second: " + describe(co_await second->end_message()));
                    }
                    finished->fetch_add(1);
                    sched->notify_all(*finished);
                }(&*conn, &out, &sched, &finished));
            (void)co_await sched.sleep_until(net::io::clock::now() + 50ms);

            out.add("ping: " + describe(co_await conn->ping(std::as_bytes(std::span{"between"sv}))));
            out.add("too long: " + describe(co_await conn->ping(std::vector<std::byte>(126))));
            (void)co_await streamed->write("two"sv);
            out.add("end: " + describe(co_await streamed->end_message()));
            out.add("ended: " + describe(co_await streamed->end_message()));

            const auto deadline = net::io::clock::now() + 5s;
            while (finished.load() < 2)
            {
                if (co_await sched.wait_on(finished, finished.load(), deadline)) break;
            }
            (void)co_await conn->close();
        });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        while (true)
        {
            auto f = co_await peer.read_frame();
            if (!f.has_value()) break;
            const bool closing = f->header.op == opcode::close;
            result.frames.push_back(std::move(*f));
            if (closing) break;
        }

        (void)co_await peer.send(close_frame(ws::close_code::normal_closure));
        (void)co_await peer.read_until_end();
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    auto       frames = summaries(result.frames);
    CAPTURE(frames);

    // the whole message and the second streamed one come after the first's end, in either order.
    REQUIRE(frames.size() == 8);
    CHECK(std::vector(frames.begin(), frames.begin() + 4)
          == std::vector<std::string>{"text+:one", "ping:between", "cont+:two", "cont:"});
    const std::vector<std::string> second{"text+:second", "cont:"};
    const bool                     whole_first = frames[4] == "binary:whole";
    CHECK(std::vector(frames.begin() + (whole_first ? 5 : 4), frames.begin() + (whole_first ? 7 : 6)) == second);
    CHECK(frames[whole_first ? 4 : 6] == "binary:whole");
    CHECK(frames.back() == "close:1000");

    const auto               invalid = std::make_error_code(std::errc::invalid_argument).message();
    std::vector<std::string> expected{"end: ok",
                                      "ended: " + invalid,
                                      "ping: ok",
                                      "second: ok",
                                      "too long: " + invalid,
                                      "whole: ok"};
    auto                     lines = out.get();
    std::ranges::sort(lines);
    std::ranges::sort(expected);
    CHECK(lines == expected);
}

TEST_CASE("a WebSocket answers only the latest ping while a write holds the connection", "[websocket][server][write]")
{
    record                     out;
    std::atomic<std::uint64_t> pinged = 0;
    std::atomic<std::uint64_t> wrote  = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    // larger than the sockets' buffers, so the write waits for the peer to read.
    const std::string large(32 << 20, 'l');

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw);
                   if (!conn.has_value()) co_return;

                   (void)sched.spawn(
                       [](ws::connection*             conn,
                          const std::string*          large,
                          record*                     out,
                          net::io::scheduler*         sched,
                          std::atomic<std::uint64_t>* wrote) -> task<void>
                       {
                           out->add("large: " + describe(co_await conn->write_message(ws::message_type::text, *large)));
                           wrote->store(1);
                           sched->notify_all(*wrote);
                       }(&*conn, &large, &out, &sched, &wrote));

                   // the pings come before this message, so they've been read once it is: with the write still
                   // waiting, as the peer doesn't read until it's told.
                   std::string message;
                   const auto  read = co_await conn->read_message(message);
                   out.add("read: " + (read.has_value() ? message : describe(read.error())));
                   pinged.store(1);
                   sched.notify_all(pinged);

                   const auto deadline = net::io::clock::now() + 10s;
                   while (wrote.load() == 0)
                   {
                       if (co_await sched.wait_on(wrote, 0, deadline)) break;
                   }
                   (void)co_await conn->close();
               });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> frames;
        auto                     conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return frames;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        if (!(co_await peer.read_head()).has_value()) co_return frames;

        (void)co_await peer.send(join(client_frame(opcode::ping, "1"sv),
                                      client_frame(opcode::ping, "2"sv),
                                      client_frame(opcode::ping, "3"sv),
                                      client_frame(opcode::text, "after"sv)));

        const auto deadline = net::io::clock::now() + 10s;
        while (pinged.load() == 0)
        {
            if (co_await sched.wait_on(pinged, 0, deadline)) break;
        }

        while (true)
        {
            auto f = co_await peer.read_frame();
            if (!f.has_value()) break;
            const bool closing = f->header.op == opcode::close;
            frames.push_back(f->header.op == opcode::text && f->payload.size() == large.size() && f->text() == large
                                 ? "large"
                                 : summary(*f));
            if (closing) break;
        }

        (void)co_await peer.send(close_frame(ws::close_code::normal_closure));
        (void)co_await peer.read_until_end();
        co_return frames;
    };

    const auto frames = sched.run_to_completion(client());
    CHECK(frames == std::vector<std::string>{"large", "pong:3", "close:1000"});
    CHECK(out.get() == std::vector<std::string>{"read: after", "large: ok"});
}

TEST_CASE("a WebSocket's write after its close has gone fails", "[websocket][server][close]")
{
    record                     out;
    std::atomic<std::uint64_t> closing = 0;
    std::atomic<std::uint64_t> wrote   = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw);
                   if (!conn.has_value()) co_return;

                   auto streamed = co_await conn->begin_message(ws::message_type::text);
                   if (!streamed.has_value()) co_return;
                   (void)co_await streamed->write("part"sv);

                   // close() waits for the peer's close, which waits for the writes below.
                   (void)sched.spawn(
                       [](ws::connection*             conn,
                          record*                     out,
                          net::io::scheduler*         sched,
                          std::atomic<std::uint64_t>* closing) -> task<void>
                       {
                           out->add("close: " + describe(co_await conn->close()));
                           closing->store(2);
                           sched->notify_all(*closing);
                       }(&*conn, &out, &sched, &closing));

                   const auto deadline = net::io::clock::now() + 5s;
                   while (closing.load() == 0)
                   {
                       if (co_await sched.wait_on(closing, 0, deadline)) break;
                   }

                   out.add("message: " + describe(co_await conn->write_message(ws::message_type::text, "late"sv)));
                   const auto part = co_await streamed->write("late"sv);
                   out.add("part: " + describe(part.err));
                   out.add("end: " + describe(co_await streamed->end_message()));
                   wrote.store(1);
                   sched.notify_all(wrote);

                   while (closing.load() != 2)
                   {
                       if (co_await sched.wait_on(closing, closing.load(), deadline)) break;
                   }
               });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        for (int i = 0; i < 2; ++i)
        {
            auto f = co_await peer.read_frame();
            if (!f.has_value()) co_return result;
            result.frames.push_back(std::move(*f));
        }

        // the server's close has come: the handler writes, then the answer goes.
        closing.store(1);
        sched.notify_all(closing);
        const auto deadline = net::io::clock::now() + 5s;
        while (wrote.load() == 0)
        {
            if (co_await sched.wait_on(wrote, 0, deadline)) break;
        }

        (void)co_await peer.send(close_frame(ws::close_code::normal_closure));
        auto [rest, err] = co_await peer.read_until_end();
        result.frames.insert(result.frames.end(), rest.begin(), rest.end());
        result.err = err;
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(summaries(result.frames) == std::vector<std::string>{"text+:part", "close:1000"});
    CHECK(result.err == net::io::errc::end_of_stream);

    const auto pipe = std::make_error_code(std::errc::broken_pipe).message();
    CHECK(out.get() == std::vector<std::string>{"message: " + pipe, "part: " + pipe, "end: " + pipe, "close: ok"});
}

TEST_CASE("a WebSocket's writes to a peer that doesn't read fail at the server's write_timeout",
          "[websocket][server][write]")
{
    record                     out;
    std::atomic<std::uint64_t> failed = 0;
    std::atomic<std::uint64_t> read   = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw);
                   if (!conn.has_value()) co_return;

                   // a read waiting meanwhile, for a message that never comes: the failed write ends it.
                   std::error_code read_err;
                   (void)sched.spawn(
                       [](ws::connection*             conn,
                          std::error_code*            err,
                          net::io::scheduler*         sched,
                          std::atomic<std::uint64_t>* read) -> task<void>
                       {
                           std::string message;
                           *err = (co_await conn->read_message(message)).error_or(std::error_code{});
                           read->store(1);
                           sched->notify_all(*read);
                       }(&*conn, &read_err, &sched, &read));

                   const std::string message(1 << 20, 'x');
                   std::error_code   err;
                   for (int i = 0; i < 1'000 && !err; ++i)
                       err = co_await conn->write_message(ws::message_type::text, message);

                   out.add(std::format("write timed out: {}", err == std::errc::timed_out));
                   out.add(std::format("then: {}", (co_await conn->ping()) == std::errc::timed_out));

                   const auto deadline = net::io::clock::now() + 5s;
                   while (read.load() == 0)
                   {
                       if (co_await sched.wait_on(read, 0, deadline)) break;
                   }
                   out.add(std::format("read: {}", read_err == std::errc::timed_out));
                   failed.store(1);
                   sched.notify_all(failed);
               });
    auto server = make_server(&sched, std::move(router), {.write_timeout = 1s});

    const auto client = [&] -> task<void>
    {
        auto conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        (void)co_await peer.read_head();

        const auto deadline = net::io::clock::now() + 10s;
        while (failed.load() == 0)
        {
            if (co_await sched.wait_on(failed, 0, deadline)) break;
        }
    };

    sched.run_to_completion(client());
    CHECK(out.get() == std::vector<std::string>{"write timed out: true", "then: true", "read: true"});
}

TEST_CASE("an idle WebSocket pings the peer at half its idle_timeout, and fails at its end",
          "[websocket][server][idle]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw)
               { return echo(req, rw, out, {.connection = {.idle_timeout = 400ms}}); });
    auto server = make_server(&sched, std::move(router));

    struct times
    {
        net::io::clock::duration ping{};
        net::io::clock::duration end{};
    };

    const auto client = [&] -> task<std::pair<peer_result, times>>
    {
        peer_result result;
        times       took;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{result, took};

        // from before the request, so the server's first read began after.
        const auto start = net::io::clock::now();
        raw_peer   peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        // the ping, which isn't answered.
        auto ping = co_await peer.read_frame();
        if (!ping.has_value()) co_return std::pair{result, took};
        took.ping = net::io::clock::now() - start;
        result.frames.push_back(std::move(*ping));

        auto [rest, err] = co_await peer.read_until_end();
        took.end         = net::io::clock::now() - start;
        result.frames.insert(result.frames.end(), rest.begin(), rest.end());
        result.err = err;
        co_return std::pair{result, took};
    };

    const auto [result, took] = sched.run_to_completion(client());
    CHECK(summaries(result.frames) == std::vector<std::string>{"ping:"});
    CHECK(took.ping >= 200ms);

    // aborted: the connection ends without a close.
    CHECK(result.err == net::io::errc::end_of_stream);
    CHECK(took.end >= 400ms);

    const auto timed_out = std::make_error_code(std::errc::timed_out).message();
    CHECK(out.get()
          == std::vector<std::string>{"read: " + timed_out + " reason=",
                                      "then write: " + timed_out,
                                      "then close: " + timed_out});
}

TEST_CASE("a WebSocket's idle wait starts over with a pong, or anything the peer sends", "[websocket][server][idle]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // whether the peer answers the ping with its pong, or with a message.
    const bool message = GENERATE(false, true);
    CAPTURE(message);

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw)
               { return echo(req, rw, out, {.connection = {.idle_timeout = 400ms}}); });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::pair<peer_result, net::io::clock::duration>>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{result, net::io::clock::duration{}};

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        auto ping = co_await peer.read_frame();
        if (!ping.has_value()) co_return std::pair{result, net::io::clock::duration{}};
        result.frames.push_back(std::move(*ping));

        // without the answer, the server would end the connection when the other half passes, rather than ping again
        // a half after it.
        const auto answered = net::io::clock::now();
        (void)co_await peer.send(message ? client_frame(opcode::text, "here"sv) : client_frame(opcode::pong, ""sv));

        const auto frames = message ? 2 : 1;
        for (int i = 0; i < frames; ++i)
        {
            auto f = co_await peer.read_frame();
            if (!f.has_value()) break;
            result.frames.push_back(std::move(*f));
        }
        const auto again = net::io::clock::now() - answered;

        auto [rest, err] = co_await peer.read_until_end();
        result.frames.insert(result.frames.end(), rest.begin(), rest.end());
        result.err = err;
        co_return std::pair{result, again};
    };

    const auto [result, again] = sched.run_to_completion(client());
    CHECK(summaries(result.frames)
          == (message ? std::vector<std::string>{"ping:", "text:here", "ping:"}
                      : std::vector<std::string>{"ping:", "ping:"}));
    CHECK(again >= 200ms);
    CHECK(result.err == net::io::errc::end_of_stream);
    CHECK(out.get().front() == "read: " + std::make_error_code(std::errc::timed_out).message() + " reason=");
}

TEST_CASE("a WebSocket's idle wait starts over as a large frame trickles in", "[websocket][server][idle]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw)
               { return echo(req, rw, out, {.connection = {.idle_timeout = 400ms}}); });
    auto server = make_server(&sched, std::move(router));

    // a frame larger than the connection's buffer is read straight into the message, once what's buffered is taken:
    // the idle timeout times those reads as any.
    const std::string large(std::size_t{64} * 1'024, 'x');
    const auto        frame = client_frame(opcode::text, large);
    const auto        half  = frame.size() / 2;

    const auto client = [&] -> task<std::pair<peer_result, bool>>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{result, false};

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        // the frame's first part, a ping for the wait after it, then the next part, and a ping again after that (the
        // connection would have ended there, without the part).
        const auto bytes = std::span{frame};
        (void)co_await peer.send(bytes.first(1'024));
        for (const auto& [from, to] : {
                 std::pair{std::size_t{1'024},         half},
                 std::pair{              half, frame.size()}
        })
        {
            auto ping = co_await peer.read_frame();
            if (!ping.has_value()) co_return std::pair{result, false};
            result.frames.push_back(std::move(*ping));
            (void)co_await peer.send(bytes.subspan(from, to - from));
        }

        auto echoed = co_await peer.read_frame();
        if (!echoed.has_value()) co_return std::pair{result, false};
        const bool same = echoed->text() == large;

        (void)co_await peer.send(close_frame(ws::close_code::normal_closure));
        auto [rest, err] = co_await peer.read_until_end();
        result.frames.insert(result.frames.end(), rest.begin(), rest.end());
        result.err = err;
        co_return std::pair{result, same};
    };

    const auto [result, same] = sched.run_to_completion(client());
    CHECK(summaries(result.frames) == std::vector<std::string>{"ping:", "ping:", "close:1000"});
    CHECK(same);
    CHECK(result.err == net::io::errc::end_of_stream);
    CHECK(out.get().front() == "read: 1000 peer reason=");
}

TEST_CASE("a WebSocket whose idle_timeout is 0 waits as long as the peer's quiet", "[websocket][server][idle]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw)
               { return echo(req, rw, out, {.connection = {.idle_timeout = 0ms}}); });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        (void)co_await sched.sleep_until(net::io::clock::now() + 300ms);
        (void)co_await peer.send(
            join(client_frame(opcode::text, "late"sv), close_frame(ws::close_code::normal_closure)));

        auto [frames, err] = co_await peer.read_until_end();
        result.frames      = std::move(frames);
        result.err         = err;
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(summaries(result.frames) == std::vector<std::string>{"text:late", "close:1000"});
    CHECK(result.err == net::io::errc::end_of_stream);
    CHECK(out.get().front() == "read: 1000 peer reason=");
}

TEST_CASE("stopping a WebSocket's stop token cancels its read, and aborts it", "[websocket][server][cancel]")
{
    record             out;
    std::stop_source   stop;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    // whether stop's requested before the handler accepts, or as the WebSocket reads.
    const bool before = GENERATE(false, true);
    CAPTURE(before);

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw)
               {
                   if (before) stop.request_stop();
                   return echo(req, rw, out, {.stop = stop.get_token()});
               });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        if (!before)
        {
            (void)co_await sched.sleep_until(net::io::clock::now() + 100ms);
            stop.request_stop();
        }

        auto [frames, err] = co_await peer.read_until_end();
        result.frames      = std::move(frames);
        result.err         = err;
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(result.head.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(result.frames.empty());
    CHECK(result.err == net::io::errc::end_of_stream);

    const auto canceled = std::make_error_code(std::errc::operation_canceled).message();
    CHECK(out.get()
          == std::vector<std::string>{"read: " + canceled + " reason=",
                                      "then write: " + canceled,
                                      "then close: " + canceled});
}

TEST_CASE("stopping a WebSocket's stop token after its close handshake changes nothing", "[websocket][server][cancel]")
{
    record             out;
    std::stop_source   stop;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw, {.stop = stop.get_token()});
                   if (!conn.has_value()) co_return;

                   std::string message;
                   out.add("read: " + describe((co_await conn->read_message(message)).error_or(std::error_code{})));

                   stop.request_stop();
                   out.add("then write: " + describe(co_await conn->write_message(ws::message_type::text, "x"sv)));
                   out.add("then close: " + describe(co_await conn->close()));
               });
    auto server = make_server(&sched, std::move(router));

    const auto result = sched.run_to_completion(run_peer(&sched,
                                                         server->local_port(),
                                                         test::websocket::handshake(),
                                                         close_frame(ws::close_code::normal_closure)));
    CHECK(summaries(result.frames) == std::vector<std::string>{"close:1000"});
    CHECK(result.err == net::io::errc::end_of_stream);
    CHECK(out.get() == std::vector<std::string>{"read: 1000 peer", "then write: 1000 peer", "then close: ok"});
}

TEST_CASE("stopping a WebSocket's stop token cancels its writes", "[websocket][server][cancel]")
{
    record                     out;
    std::stop_source           stop;
    std::atomic<std::uint64_t> started = 0;
    std::atomic<std::uint64_t> whole   = 0;
    std::atomic<std::uint64_t> done    = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    // whether the write waits for the peer (which doesn't read), or for its turn, behind a message written a frame at a
    // time whose writer is idle.
    const bool for_turn = GENERATE(false, true);
    CAPTURE(for_turn);

    http::router router;
    router.GET(
        "/ws",
        [&](http::server_request& req, http::response_writer& rw) -> task<void>
        {
            auto conn = co_await ws::accept(req, rw, {.stop = stop.get_token()});
            if (!conn.has_value()) co_return;

            if (!for_turn)
            {
                raise(&sched, started);
                const std::string message(1 << 20, 'x');
                std::error_code   err;
                for (int i = 0; i < 1'000 && !err; ++i)
                    err = co_await conn->write_message(ws::message_type::binary, message);
                out.add("write: " + describe(err));
            }
            else
            {
                auto streamed = co_await conn->begin_message(ws::message_type::text);
                if (!streamed.has_value()) co_return;
                (void)co_await streamed->write("part"sv);

                (void)sched.spawn(
                    [](ws::connection* conn, record* out, net::io::scheduler* sched, std::atomic<std::uint64_t>* whole)
                        -> task<void>
                    {
                        out->add("whole: " + describe(co_await conn->write_message(ws::message_type::text, "whole"sv)));
                        raise(sched, *whole);
                    }(&*conn, &out, &sched, &whole));
                raise(&sched, started);

                if (!co_await reach(&sched, whole)) out.add("the whole message's write wasn't woken");
                out.add("end: " + describe(co_await streamed->end_message()));
            }

            out.add("then: " + describe(co_await conn->ping()));
            raise(&sched, done);
        });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::error_code>
    {
        auto conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return conn.error();

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        (void)co_await peer.read_head();

        // the write's waiting by then; and the client reads nothing until it has failed, so nothing else ends it.
        (void)co_await reach(&sched, started);
        (void)co_await sched.sleep_until(net::io::clock::now() + 200ms);
        stop.request_stop();
        if (!co_await reach(&sched, done)) out.add("the handler didn't finish");

        auto [frames, err] = co_await peer.read_until_end();
        co_return err;
    };

    const auto err = sched.run_to_completion(client());
    CHECK(err == net::io::errc::end_of_stream);

    const auto canceled = std::make_error_code(std::errc::operation_canceled).message();
    CHECK(out.get()
          == (for_turn ? std::vector<std::string>{"whole: " + canceled, "end: " + canceled, "then: " + canceled}
                       : std::vector<std::string>{"write: " + canceled, "then: " + canceled}));
}

TEST_CASE("stopping a WebSocket's stop token cancels its close", "[websocket][server][cancel]")
{
    record                     out;
    std::stop_source           stop;
    std::atomic<std::uint64_t> finished = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    // whether close() reads for the peer's close itself, or waits for another task that's reading.
    const bool other_reads = GENERATE(false, true);
    CAPTURE(other_reads);

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn =
                       co_await ws::accept(req, rw, {.connection = {.close_timeout = 5s}, .stop = stop.get_token()});
                   if (!conn.has_value()) co_return;

                   if (other_reads)
                   {
                       (void)sched.spawn(
                           [](ws::connection*             conn,
                              record*                     out,
                              net::io::scheduler*         sched,
                              std::atomic<std::uint64_t>* finished) -> task<void>
                           {
                               std::string message;
                               const auto  read = co_await conn->read_message(message);
                               out->add("reader: " + describe(read.error_or(std::error_code{})));
                               raise(sched, *finished);
                           }(&*conn, &out, &sched, &finished));

                       // the reader's waiting by then: one read at a time.
                       (void)co_await sched.sleep_until(net::io::clock::now() + 20ms);
                       std::string other;
                       const auto  busy = co_await conn->read_message(other);
                       if (busy.error_or(std::error_code{}) != std::errc::device_or_resource_busy)
                           out.add("the reader wasn't reading");
                   }

                   out.add("close: " + describe(co_await conn->close()));
                   if (other_reads && !co_await reach(&sched, finished)) out.add("the reader didn't finish");
               });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        // the close, which isn't answered: stop ends close()'s wait for it.
        auto close = co_await peer.read_frame();
        if (!close.has_value()) co_return result;
        result.frames.push_back(std::move(*close));
        stop.request_stop();

        auto [frames, err] = co_await peer.read_until_end();
        result.frames.insert(result.frames.end(), frames.begin(), frames.end());
        result.err = err;
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(summaries(result.frames) == std::vector<std::string>{"close:1000"});
    CHECK(result.err == net::io::errc::end_of_stream);

    // the reader's end, and close()'s, in either order.
    const auto canceled = std::make_error_code(std::errc::operation_canceled).message();
    auto       lines    = out.get();
    std::ranges::sort(lines);
    CHECK(lines
          == (other_reads ? std::vector<std::string>{"close: " + canceled, "reader: " + canceled}
                          : std::vector<std::string>{"close: " + canceled}));
}

TEST_CASE("http::timeout on a WebSocket's route ends the WebSocket at its deadline", "[websocket][server][cancel]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.use(http::timeout{&sched, 500ms});
    router.GET("/ws", [&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        // it echoes until the deadline; then it ends, without a 503 (the 101 was the response).
        (void)co_await peer.send(client_frame(opcode::text, "hi"sv));
        auto [frames, err] = co_await peer.read_until_end();
        result.frames      = std::move(frames);
        result.err         = err;
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(result.head.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(summaries(result.frames) == std::vector<std::string>{"text:hi"});
    CHECK(result.err == net::io::errc::end_of_stream);

    const auto canceled = std::make_error_code(std::errc::operation_canceled).message();
    CHECK(out.get()
          == std::vector<std::string>{"read: " + canceled + " reason=",
                                      "then write: " + canceled,
                                      "then close: " + canceled});
}

TEST_CASE("the server's close tells a reading WebSocket it's going away, and join() waits for the close handshake",
          "[websocket][server][close][going-away]")
{
    record                     out;
    std::atomic<std::uint64_t> joined = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws", [&](http::server_request& req, http::response_writer& rw) { return echo(req, rw, out); });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        // an echo, after which the handler reads again.
        (void)co_await peer.send(client_frame(opcode::text, "hi"sv));
        for (int i = 0; i < 2; ++i)
        {
            if (i == 1)
            {
                server->close();
                (void)sched.spawn(
                    [](http::server* server, record* out, net::io::scheduler* sched, std::atomic<std::uint64_t>* joined)
                        -> task<void>
                    {
                        co_await server->join();
                        out->add("joined");
                        raise(sched, *joined);
                    }(server.get(), &out, &sched, &joined));
            }

            auto f = co_await peer.read_frame();
            if (!f.has_value()) co_return result;
            result.frames.push_back(std::move(*f));
        }

        // join() waits for the answer.
        (void)co_await sched.sleep_until(net::io::clock::now() + 100ms);
        out.add("answering");
        (void)co_await peer.send(close_frame(ws::close_code::going_away));

        auto [rest, err] = co_await peer.read_until_end();
        result.frames.insert(result.frames.end(), rest.begin(), rest.end());
        result.err = err;

        if (!co_await reach(&sched, joined)) out.add("join() didn't return");
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(summaries(result.frames) == std::vector<std::string>{"text:hi", "close:1001"});
    CHECK(result.err == net::io::errc::end_of_stream);

    // the read reports the close it sent; the handshake's done, so close() has nothing to do.
    CHECK(out.get()
          == std::vector<std::string>{"answering",
                                      "read: 1001 sent reason=",
                                      "then write: 1001 sent",
                                      "then close: ok",
                                      "joined"});
}

TEST_CASE("the server's close gives up on a WebSocket's peer that doesn't answer within close_timeout",
          "[websocket][server][close][going-away]")
{
    record             out;
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(4)};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw)
               { return echo(req, rw, out, {.connection = {.close_timeout = 300ms}}); });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<std::pair<peer_result, net::io::clock::duration>>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return std::pair{result, net::io::clock::duration{}};

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        // the close, which isn't answered: the server aborts the connection close_timeout after it.
        const auto closed = net::io::clock::now();
        server->close();
        auto [frames, err] = co_await peer.read_until_end();
        const auto took    = net::io::clock::now() - closed;
        result.frames      = std::move(frames);
        result.err         = err;

        co_await server->join();
        co_return std::pair{result, took};
    };

    const auto [result, took] = sched.run_to_completion(client());
    CHECK(summaries(result.frames) == std::vector<std::string>{"close:1001"});
    CHECK(result.err == net::io::errc::end_of_stream);
    CHECK(took >= 300ms);
    CHECK(out.get()
          == std::vector<std::string>{"read: 1001 sent reason=", "then write: 1001 sent", "then close: 1001 sent"});
}

namespace
{

// flow is what a peer sending nonstop (send_nonstop()) and the test share: how much it has sent, when it's to stop,
// and whether it has.
struct flow
{
    std::atomic<std::uint64_t> sent = 0;
    std::atomic<std::uint64_t> stop = 0;
    std::atomic<std::uint64_t> done = 0;
};

// send_nonstop sends batch over and over, until it's told to stop or a write fails, so the server's reads always find
// something to read (the peer sends faster than they take it), and never have to wait.
task<void> send_nonstop(raw_peer* peer, std::vector<std::byte> batch, net::io::scheduler* sched, flow* f)
{
    while (f->stop.load() == 0)
    {
        if (co_await peer->send(batch)) break;
        f->sent.fetch_add(batch.size());
        sched->notify_all(f->sent);
    }
    raise(sched, f->done);
}

// batch_of is frame repeated up to about 64 KiB.
std::vector<std::byte> batch_of(std::span<const std::byte> frame)
{
    std::vector<std::byte> batch;
    while (batch.size() < std::size_t{64} * 1'024) batch.insert(batch.end(), frame.begin(), frame.end());
    return batch;
}

// What a peer that never lets the server's reads wait sends.
enum class busy_shape : std::uint8_t
{
    messages,    // small whole messages, read whole
    streamed,    // one message that never ends, of 4 KiB frames, read as a stream
    empty_parts, // one message that never ends, of empty frames (so it never reaches max_message), read whole
};

}

TEST_CASE("the server's close reaches a WebSocket whose peer never lets its reads wait",
          "[websocket][server][close][going-away][busy]")
{
    record                     out;
    std::atomic<std::uint64_t> joined = 0;
    flow                       sending;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    const auto shape = GENERATE(busy_shape::messages, busy_shape::streamed, busy_shape::empty_parts);
    // whether the peer answers the server's close (having stopped sending once it has seen it), or sends on, never
    // answering.
    const bool answers = GENERATE(true, false);
    CAPTURE(shape, answers);

    const auto close_timeout = answers ? std::chrono::milliseconds{5s} : std::chrono::milliseconds{300ms};

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(
                       req,
                       rw,
                       {
                           .connection = {.max_message = std::uint64_t{1} << 62, .close_timeout = close_timeout}
                   });
                   if (!conn.has_value()) co_return;

                   std::error_code err;
                   if (shape == busy_shape::streamed)
                   {
                       const auto type = co_await conn->next_message();
                       err             = type.error_or(std::error_code{});
                       std::array<std::byte, 4'096> buf{};
                       while (!err)
                       {
                           const auto res = co_await conn->message().read(buf);
                           err            = res.err;
                       }
                   }
                   else
                   {
                       std::string message;
                       while (!err)
                       {
                           const auto type = co_await conn->read_message(message);
                           err             = type.error_or(std::error_code{});
                       }
                   }
                   out.add("read: " + describe(err));
               });
    auto server = make_server(&sched, std::move(router));

    struct busy_result
    {
        std::string                 head;
        std::vector<received_frame> frames;
        std::error_code             err;
        net::io::clock::duration    to_end{}; // from the server's close to the connection's end
        net::io::clock::duration    to_join{};
    };

    // steps is the peer's part once it's sending nonstop.
    const auto steps = [&](raw_peer& peer, busy_result& result) -> task<void>
    {
        // the server's reads are under way, and behind.
        if (!co_await reach(&sched, sending.sent, std::uint64_t{1} << 20)) co_return;

        const auto closed = net::io::clock::now();
        server->close();
        (void)sched.spawn(
            [](http::server* server, record* out, net::io::scheduler* sched, std::atomic<std::uint64_t>* joined)
                -> task<void>
            {
                co_await server->join();
                out->add("joined");
                raise(sched, *joined);
            }(server.get(), &out, &sched, &joined));

        auto close = co_await peer.read_frame();
        if (!close.has_value())
        {
            result.err = close.error();
            co_return;
        }
        result.frames.push_back(std::move(*close));

        if (answers)
        {
            raise(&sched, sending.stop);
            if (!co_await reach(&sched, sending.done)) co_return;
            out.add("answering");
            (void)co_await peer.send(close_frame(ws::close_code::going_away));
        }

        auto [rest, err] = co_await peer.read_until_end();
        result.to_end    = net::io::clock::now() - closed;
        result.frames.insert(result.frames.end(), rest.begin(), rest.end());
        result.err = err;

        if (!co_await reach(&sched, joined)) out.add("join() didn't return");
        result.to_join = net::io::clock::now() - closed;
    };

    const auto client = [&] -> task<busy_result>
    {
        busy_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        std::vector<std::byte> batch;
        switch (shape)
        {
        case busy_shape::messages: batch = batch_of(client_frame(opcode::text, "hello"sv)); break;
        case busy_shape::streamed:
        {
            (void)co_await peer.send(client_frame(opcode::binary, "first"sv, false));
            const std::vector<std::byte> part(4'096, std::byte{'x'});
            batch = batch_of(client_frame(opcode::continuation, part, false));
            break;
        }
        case busy_shape::empty_parts:
            (void)co_await peer.send(client_frame(opcode::text, "first"sv, false));
            batch = batch_of(client_frame(opcode::continuation, ""sv, false));
            break;
        }

        if (!sched.spawn(send_nonstop(&peer, std::move(batch), &sched, &sending))) co_return result;
        co_await steps(peer, result);

        // the sender's done with the connection before it goes (its writes fail, once the server has ended it).
        raise(&sched, sending.stop);
        if (!co_await reach(&sched, sending.done)) out.add("the sender didn't stop");
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(result.head.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(summaries(result.frames) == std::vector<std::string>{"close:1001"});

    if (answers)
    {
        // the handshake finishes as soon as the peer answers: well within close_timeout.
        CHECK(result.err == net::io::errc::end_of_stream);
        CHECK(result.to_join < close_timeout);
        CHECK(out.get() == std::vector<std::string>{"answering", "read: 1001 sent", "joined"});
    }
    else
    {
        // a peer that sends on is cut off at close_timeout, however much it sends.
        CHECK(result.err == net::io::status_condition::closed);
        CHECK(result.to_end >= close_timeout);
        CHECK(out.get() == std::vector<std::string>{"read: 1001 sent", "joined"});
    }
}

TEST_CASE("a WebSocket that isn't reading when the server closes goes away at its next read, whatever's buffered",
          "[websocket][server][close][going-away]")
{
    record                     out;
    std::atomic<std::uint64_t> ready = 0;
    std::atomic<std::uint64_t> go    = 0;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    // whether the server closes before the handler accepts, or after.
    const bool before = GENERATE(false, true);
    // whether the peer sent a message before the server closed: it's dropped, as what comes after is.
    const bool message = GENERATE(false, true);
    CAPTURE(before, message);

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   if (before)
                   {
                       raise(&sched, ready);
                       if (!co_await reach(&sched, go)) co_return;
                   }

                   auto conn = co_await ws::accept(req, rw);
                   if (!conn.has_value()) co_return;

                   if (!before)
                   {
                       raise(&sched, ready);
                       if (!co_await reach(&sched, go)) co_return;
                   }

                   std::string message;
                   const auto  read = co_await conn->read_message(message);
                   out.add("read: " + describe(read.error_or(std::error_code{})));
               });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        if (!before) result.head = (co_await peer.read_head()).value_or("");
        if (message) (void)co_await peer.send(client_frame(opcode::text, "early"sv));

        (void)co_await reach(&sched, ready);
        server->close();

        if (!before)
        {
            // nothing's said until the handler reads.
            conn->set_read_deadline(net::io::clock::now() + 200ms);
            const auto quiet = co_await peer.read_frame();
            if (quiet.error_or(std::error_code{}) != std::errc::timed_out) out.add("a frame came before the read");
            conn->set_read_deadline(std::nullopt);
        }

        raise(&sched, go);
        if (before) result.head = (co_await peer.read_head()).value_or("");

        auto close = co_await peer.read_frame();
        if (!close.has_value()) co_return result;
        result.frames.push_back(std::move(*close));

        (void)co_await peer.send(close_frame(ws::close_code::going_away));
        auto [rest, err] = co_await peer.read_until_end();
        result.frames.insert(result.frames.end(), rest.begin(), rest.end());
        result.err = err;
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(result.head.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    CHECK(summaries(result.frames) == std::vector<std::string>{"close:1001"});
    CHECK(result.err == net::io::errc::end_of_stream);
    CHECK(out.get() == std::vector<std::string>{"read: 1001 sent"});
}

TEST_CASE("the server's close leaves a WebSocket's close in progress to finish",
          "[websocket][server][close][going-away]")
{
    record                     out;
    std::atomic<std::uint64_t> finished = 0;
    flow                       sending;
    net::io::scheduler         sched{std::make_shared<net::coro::thread_pool>(4)};

    // whether close() reads for the peer's close itself, or waits for another task that's reading.
    const bool other_reads = GENERATE(false, true);
    // whether the peer sends nonstop as the server closes (so the reads never wait), until it answers.
    const bool busy = GENERATE(false, true);
    CAPTURE(other_reads, busy);

    http::router router;
    router.GET("/ws",
               [&](http::server_request& req, http::response_writer& rw) -> task<void>
               {
                   auto conn = co_await ws::accept(req, rw, {.connection = {.close_timeout = 5s}});
                   if (!conn.has_value()) co_return;

                   if (other_reads)
                   {
                       (void)sched.spawn(
                           [](ws::connection*             conn,
                              record*                     out,
                              net::io::scheduler*         sched,
                              std::atomic<std::uint64_t>* finished) -> task<void>
                           {
                               std::string message;
                               const auto  read = co_await conn->read_message(message);
                               out->add("reader: " + describe(read.error_or(std::error_code{})));
                               raise(sched, *finished);
                           }(&*conn, &out, &sched, &finished));
                       (void)co_await sched.sleep_until(net::io::clock::now() + 20ms);
                   }

                   out.add("close: " + describe(co_await conn->close(ws::close_code{4000})));
                   if (other_reads && !co_await reach(&sched, finished)) out.add("the reader didn't finish");
               });
    auto server = make_server(&sched, std::move(router));

    const auto client = [&] -> task<peer_result>
    {
        peer_result result;
        auto        conn = co_await connect(&sched, server->local_port());
        if (!conn.has_value()) co_return result;

        raw_peer peer{*conn, *conn};
        (void)co_await peer.send(test::websocket::handshake());
        result.head = (co_await peer.read_head()).value_or("");

        // the handler's close is waiting for the answer as the server closes: then the answer comes.
        auto close = co_await peer.read_frame();
        if (!close.has_value()) co_return result;
        result.frames.push_back(std::move(*close));

        if (busy)
        {
            if (!sched.spawn(send_nonstop(&peer, batch_of(client_frame(opcode::text, "hello"sv)), &sched, &sending)))
                co_return result;
            (void)co_await reach(&sched, sending.sent, std::uint64_t{1} << 20);
        }

        server->close();
        (void)co_await sched.sleep_until(net::io::clock::now() + 100ms);
        if (busy)
        {
            raise(&sched, sending.stop);
            if (!co_await reach(&sched, sending.done)) co_return result;
        }
        (void)co_await peer.send(close_frame(ws::close_code{4001}));

        auto [rest, err] = co_await peer.read_until_end();
        result.frames.insert(result.frames.end(), rest.begin(), rest.end());
        result.err = err;
        co_return result;
    };

    const auto result = sched.run_to_completion(client());
    CHECK(summaries(result.frames) == std::vector<std::string>{"close:4000"});
    CHECK(result.err == net::io::errc::end_of_stream);

    auto lines = out.get();
    std::ranges::sort(lines);
    CHECK(lines
          == (other_reads ? std::vector<std::string>{"close: ok", "reader: 4001 peer"}
                          : std::vector<std::string>{"close: ok"}));
}
