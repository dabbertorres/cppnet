#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "tls/botan_peer.hpp"
#include "tls/test_pki.hpp"
#include "tls/tls.hpp"
#include "tls/version.hpp"
#include "util/defer.hpp"

namespace http = net::http;
namespace tls  = net::tls;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::pki;
using test::server_with;
using test::trusting_client;

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// refused is the error of the side that refused a renegotiation: the fatal no_renegotiation alert (100) it sent.
const std::error_code refused{tls::sent_alert_base + 100, tls::category()};

// no_renegotiation is that alert, as the peer that tried to renegotiate received it.
constexpr test::alert_seen no_renegotiation{.code = 100, .fatal = true};

// wait_for waits (up to 5 s) until word isn't 0, returning whether it isn't.
task<bool> wait_for(net::io::scheduler* sched, std::atomic<std::uint64_t>* word)
{
    const auto deadline = net::io::clock::now() + 5s;
    while (word->load() == 0)
    {
        if (co_await sched->wait_on(*word, 0, deadline)) break;
    }
    co_return word->load() != 0;
}

// raise sets word, for wait_for().
void raise(net::io::scheduler* sched, std::atomic<std::uint64_t>* word)
{
    word->store(1);
    sched->notify_all(*word);
}

// cppnet_side is how cppnet's end of a connection went: why its reads stopped, and a write's error after that.
struct cppnet_side
{
    std::string     failure; // what failed before the renegotiation, if anything
    std::string     echoed;  // a client's: what the server echoed
    tls::version    version = tls::version::none;
    std::error_code read;
    std::error_code write;
};

// botan_side is how the Botan peer's end went.
struct botan_side
{
    std::string                     failure; // what failed before or as it renegotiated, if anything
    std::string                     botan;   // what Botan threw, if anything
    std::string                     received;
    std::uint16_t                   version = 0;
    std::optional<test::alert_seen> alert;
    std::error_code                 end; // why its reads stopped, after the renegotiation

    void take(const test::botan_peer& peer)
    {
        botan    = peer.failure;
        received = peer.received;
        version  = peer.version;
        alert    = peer.alert;
    }
};

// h2_request is an HTTP/2 client's preface, its (empty) SETTINGS, and a GET of /wait for localhost on stream 1 (its
// header block: :method GET and :scheme https indexed, :path and :authority literals without indexing).
constexpr auto h2_request = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
                            "\x00\x00\x00\x04\x00\x00\x00\x00\x00"
                            "\x00\x00\x14\x01\x05\x00\x00\x00\x01"
                            "\x82\x87\x04\x05/wait\x01\x09localhost"sv;

// settings_acked is whether the HTTP/2 frames an HTTP/2 server sent (in received, from the first) include its
// acknowledgement of the client's SETTINGS.
bool settings_acked(std::string_view received)
{
    while (received.size() >= 9)
    {
        const auto length = (std::size_t{static_cast<std::uint8_t>(received[0])} << 16U)
                          | (std::size_t{static_cast<std::uint8_t>(received[1])} << 8U)
                          | std::size_t{static_cast<std::uint8_t>(received[2])};
        if (received[3] == 0x4 && (static_cast<std::uint8_t>(received[4]) & 0x1U) != 0) return true;
        if (received.size() < 9 + length) return false;
        received.remove_prefix(9 + length);
    }
    return false;
}

}

TEST_CASE("a TLS 1.2 client's renegotiation ends the connection with a fatal no_renegotiation alert",
          "[tls][renegotiation]")
{
    // NOTE: before the scheduler, as the server's task sets them.
    cppnet_side                server;
    std::atomic<std::uint64_t> server_done{0};

    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // echoes what it reads until its reads fail, then writes once more.
    const auto serve = [](net::io::scheduler*         sched,
                          net::listener*              listener,
                          tls::server_context         ctx,
                          cppnet_side*                out,
                          std::atomic<std::uint64_t>* done) -> task<>
    {
        // NOTE: once the stream's gone (declared before it).
        net::util::defer finished{[&] noexcept { raise(sched, done); }};

        auto accepted = co_await listener->accept();
        if (!accepted.has_value())
        {
            out->failure = "accept: " + accepted.error().message();
            co_return;
        }

        auto s = co_await tls::stream::accept(std::move(*accepted), ctx, net::io::clock::now() + 5s);
        if (!s.has_value())
        {
            out->failure = "handshake: " + s.error().message();
            co_return;
        }
        out->version = s->version();

        std::array<std::byte, 64> buf{};
        while (true)
        {
            auto res = co_await s->read(buf);
            if (res.count > 0)
            {
                if (auto echoed = co_await net::io::write_all(*s, std::span{buf}.first(res.count)); echoed.err)
                {
                    out->failure = "echo: " + echoed.err.message();
                    co_return;
                }
            }
            if (res.err)
            {
                out->read = res.err;
                break;
            }
        }

        out->write = (co_await net::io::write_all(*s, "more"sv)).err;
        co_await s->close();
    };
    REQUIRE(sched.spawn(
        serve(&sched, &listener, server_with(pki().localhost_chain, pki().localhost_key), &server, &server_done)));

    // says hello, which the server echoes, then renegotiates, and reads what comes until the connection ends.
    const auto client =
        [](net::io::scheduler* sched, std::uint16_t port, std::atomic<std::uint64_t>* server_done) -> task<botan_side>
    {
        botan_side out;

        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
        if (!conn.has_value())
        {
            out.failure = "connect: " + conn.error().message();
            co_await wait_for(sched, server_done);
            co_return out;
        }

        auto peer   = test::botan_peer::client(std::move(*conn));
        out.failure = co_await peer->handshake();
        if (out.failure.empty()) out.failure = co_await peer->send("hello");
        if (out.failure.empty()) out.failure = co_await peer->receive(5);
        if (out.failure.empty()) out.failure = co_await peer->renegotiate();
        if (out.failure.empty()) out.end = co_await peer->read_to_end();
        out.take(*peer);

        co_await wait_for(sched, server_done);
        co_return out;
    };

    const auto res = sched.run_to_completion(client(&sched, listener.local_port(), &server_done));
    CAPTURE(res.failure, res.botan, res.end.message(), server.failure, server.read.message(), server.write.message());
    REQUIRE(res.failure.empty());
    REQUIRE(server_done.load() == 1);

    CHECK(res.version == 0x0303);
    CHECK(server.version == tls::version::tls12);
    CHECK(res.received == "hello");

    // the server refused it with a fatal alert, and its stream failed: its reads, and writes after them.
    CHECK(server.failure.empty());
    CHECK(server.read == refused);
    CHECK_FALSE(tls::from_peer(server.read));
    CHECK(server.write == refused);

    // the client got the alert, then the end of the connection.
    CHECK(res.alert == no_renegotiation);
    CHECK(res.end == net::io::errc::end_of_stream);
}

TEST_CASE("a TLS 1.2 server's request to renegotiate ends the connection with a fatal no_renegotiation alert",
          "[tls][renegotiation]")
{
    // NOTE: before the scheduler, as the server's task sets them.
    botan_side                 server;
    std::atomic<std::uint64_t> server_done{0};

    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // echoes the client's hello, then asks it to renegotiate (a HelloRequest), and reads what comes until the
    // connection ends.
    const auto serve = [](net::io::scheduler*          sched,
                          net::listener*               listener,
                          const std::filesystem::path* chain,
                          const std::filesystem::path* key,
                          botan_side*                  out,
                          std::atomic<std::uint64_t>*  done) -> task<>
    {
        net::util::defer finished{[&] noexcept { raise(sched, done); }};

        auto accepted = co_await listener->accept();
        if (!accepted.has_value())
        {
            out->failure = "accept: " + accepted.error().message();
            co_return;
        }

        auto peer    = test::botan_peer::server(std::move(*accepted), *chain, *key);
        out->failure = co_await peer->handshake();
        if (out->failure.empty()) out->failure = co_await peer->receive(5);
        if (out->failure.empty()) out->failure = co_await peer->send(peer->received);
        if (out->failure.empty()) out->failure = co_await peer->renegotiate();
        if (out->failure.empty()) out->end = co_await peer->read_to_end();
        out->take(*peer);
    };
    REQUIRE(sched.spawn(serve(&sched, &listener, &pki().localhost_chain, &pki().localhost_key, &server, &server_done)));

    // says hello, reads the echo, then reads on (as the HelloRequest comes), and writes once more.
    const auto client = [](net::io::scheduler*         sched,
                           std::uint16_t               port,
                           tls::client_context         ctx,
                           std::atomic<std::uint64_t>* server_done) -> task<cppnet_side>
    {
        cppnet_side out;

        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
        if (!conn.has_value())
        {
            out.failure = "connect: " + conn.error().message();
            co_await wait_for(sched, server_done);
            co_return out;
        }

        auto s = co_await tls::stream::connect(std::move(*conn), ctx, "localhost", net::io::clock::now() + 5s);
        if (!s.has_value())
        {
            out.failure = "handshake: " + s.error().message();
            co_await wait_for(sched, server_done);
            co_return out;
        }
        out.version = s->version();

        if (auto said = co_await net::io::write_all(*s, "hello"sv); said.err)
            out.failure = "write: " + said.err.message();

        std::array<std::byte, 64> buf{};
        while (out.failure.empty() && out.echoed.size() < 5)
        {
            auto res = co_await s->read(buf);
            out.echoed.append(reinterpret_cast<const char*>(buf.data()), res.count);
            if (res.err) out.failure = "read the echo: " + res.err.message();
        }

        if (out.failure.empty())
        {
            auto res = co_await s->read(buf);
            if (res.count > 0) out.failure = "read more: " + std::to_string(res.count) + " bytes";
            out.read  = res.err;
            out.write = (co_await net::io::write_all(*s, "more"sv)).err;
        }

        co_await s->close();
        co_await wait_for(sched, server_done);
        co_return out;
    };

    const auto res = sched.run_to_completion(client(&sched, listener.local_port(), trusting_client(), &server_done));
    CAPTURE(res.failure, res.read.message(), res.write.message(), server.failure, server.botan, server.end.message());
    REQUIRE(res.failure.empty());
    REQUIRE(server_done.load() == 1);

    CHECK(res.version == tls::version::tls12);
    CHECK(server.version == 0x0303);
    CHECK(res.echoed == "hello");

    // the client refused it with a fatal alert, and its stream failed: its reads, and writes after them.
    CHECK(res.read == refused);
    CHECK_FALSE(tls::from_peer(res.read));
    CHECK(res.write == refused);

    // the server got the alert, then the end of the connection.
    CHECK(server.failure.empty());
    CHECK(server.alert == no_renegotiation);
    CHECK(server.end == net::io::errc::end_of_stream);
}

TEST_CASE("a TLS 1.2 client's renegotiation ends its HTTP connection, stopping the handler",
          "[tls][http][server][renegotiation]")
{
    // over HTTP/1.1 (the server's watch reads, as the handler waits), and HTTP/2 (RFC 9113 9.2.1: a connection error,
    // though the server can't send GOAWAY once TLS has failed).
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    // NOTE: before the scheduler, as the handler sets them.
    std::atomic<std::uint64_t> started{0};
    std::atomic<std::uint64_t> handled{0};
    std::error_code            slept;

    net::io::scheduler sched{make_pool()};

    // waits (5 s at most) until its request's stopped, answering nothing.
    http::router router;
    router.GET("/wait",
               [&](const http::server_request& req, http::response_writer& /*resp*/) -> task<void>
               {
                   raise(&sched, &started);
                   slept = co_await sched.sleep_until(net::io::clock::now() + 5s, req.stop);
                   raise(&sched, &handled);
               });

    http::server server{
        &sched,
        net::listener::make(&sched, "127.0.0.1", "0").value(),
        std::move(router),
        http::server_config{.header_read_timeout = 5s, .tls = server_with(pki().localhost_chain, pki().localhost_key)}
    };
    REQUIRE(sched.spawn(server.serve()));

    // sends its request, renegotiates once the handler's waiting, and reads what comes until the connection ends.
    const auto client = [&] -> task<botan_side>
    {
        botan_side out;

        auto conn = co_await net::tcp_socket::connect(&sched, "127.0.0.1", std::to_string(server.local_port()));
        if (!conn.has_value())
        {
            out.failure = "connect: " + conn.error().message();
            co_return out;
        }

        auto peer   = test::botan_peer::client(std::move(*conn), {http2 ? "h2" : "http/1.1"});
        out.failure = co_await peer->handshake();
        if (out.failure.empty())
            out.failure = co_await peer->send(http2 ? h2_request : "GET /wait HTTP/1.1\r\nHost: localhost\r\n\r\n"sv);
        if (out.failure.empty())
        {
            const bool waiting = co_await wait_for(&sched, &started);
            if (!waiting) out.failure = "the handler didn't start";
        }

        // NOTE: Botan fails (with unexpected_message) on application data while it's renegotiating, so the HTTP/2
        //       server's opening frames are read first, up to its acknowledgement of the client's SETTINGS: then it has
        //       nothing more to send until the handler answers.
        while (http2 && out.failure.empty() && !settings_acked(peer->received))
            out.failure = co_await peer->receive(peer->received.size() + 1);
        if (out.failure.empty()) out.failure = co_await peer->renegotiate();
        if (out.failure.empty()) out.end = co_await peer->read_to_end();
        out.take(*peer);

        const bool returned = co_await wait_for(&sched, &handled);
        if (!returned && out.failure.empty()) out.failure = "the handler didn't return";
        co_return out;
    };

    const auto res = sched.run_to_completion(client());
    CAPTURE(res.failure, res.botan, res.end.message(), slept.message());
    REQUIRE(res.failure.empty());

    CHECK(res.version == 0x0303);

    // the handler was stopped, as its connection failed.
    CHECK(slept == std::errc::operation_canceled);

    // the server refused the renegotiation with a fatal alert, and closed the connection, without a response (nor, over
    // HTTP/2, a GOAWAY: only the server's SETTINGS and the like came before the alert).
    CHECK(res.alert == no_renegotiation);
    CHECK(res.end == net::io::errc::end_of_stream);
    if (!http2) CHECK(res.received.empty());
}
