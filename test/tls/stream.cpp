#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
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
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "tls/test_pki.hpp"
#include "tls/tls.hpp"

namespace tls = net::tls;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::pki;
using test::server_with;
using test::trusting_client;

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

std::string as_string(std::span<const std::byte> data) { return {reinterpret_cast<const char*>(data.data()), data.size()}; }

// read_to_end reads s until it ends (or fails), returning what it read and why it stopped.
task<std::pair<std::string, std::error_code>> read_to_end(net::io::reader& s)
{
    std::string                  out;
    std::array<std::byte, 4'096> buf{};
    while (true)
    {
        auto res = co_await s.read(buf);
        out += as_string(std::span{buf}.first(res.count));
        if (res.err) co_return std::pair{out, res.err};
    }
}

// server_outcome is how a server's task (e.g. its handshake) ended, for a test to wait for (with wait()).
struct server_outcome
{
    net::io::scheduler*        sched;
    std::error_code            err;
    std::atomic<std::uint64_t> done{0};

    void finish(std::error_code result)
    {
        err = result;
        done.store(1);
        sched->notify_all(done);
    }

    task<> wait()
    {
        const auto deadline = net::io::clock::now() + 5s;
        while (done.load() == 0)
        {
            if (co_await sched->wait_on(done, 0, deadline)) co_return;
        }
    }
};

// serve_once accepts a connection, runs the server's handshake, and hands the stream to serve (and how the handshake
// ended to outcome, if any).
template<typename Serve>
task<> serve_once(net::listener* listener, tls::server_context ctx, Serve serve, server_outcome* outcome = nullptr)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value()) co_return;

    auto s = co_await tls::stream::accept(std::move(*accepted), ctx, net::io::clock::now() + 5s);
    if (outcome != nullptr) outcome->finish(s.has_value() ? std::error_code{} : s.error());
    if (!s.has_value()) co_return;

    co_await serve(*s);
}

// connect_to connects to the listener over TLS, as host.
task<std::expected<tls::stream, std::error_code>>
connect_to(net::io::scheduler* sched, std::uint16_t port, tls::client_context ctx, std::string host)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    if (!conn.has_value()) co_return std::unexpected(conn.error());

    co_return co_await tls::stream::connect(std::move(*conn), ctx, std::move(host), net::io::clock::now() + 5s);
}

// echo echoes what it reads, until the stream ends, then closes it.
task<> echo(tls::stream& s)
{
    std::array<std::byte, 4'096> buf{};
    while (true)
    {
        auto res = co_await s.read(buf);
        if (res.count > 0 && (co_await net::io::write_all(s, std::span{buf}.first(res.count))).err) break;
        if (res.err) break;
    }

    co_await s.close();
}

}

TEST_CASE("a client and server talk over TLS", "[tls]")
{
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    REQUIRE(sched.spawn(serve_once(&listener, server_with(pki().localhost_chain, pki().localhost_key), echo)));

    const auto client = [](net::io::scheduler* sched, std::uint16_t port) -> task<std::string>
    {
        auto s = co_await connect_to(sched, port, trusting_client({"h2", "http/1.1"}), "localhost");
        if (!s.has_value()) co_return "connect failed: " + s.error().message();

        if (s->alpn() != "http/1.1") co_return "ALPN: '" + std::string{s->alpn()} + "'";

        // more than a record (16 KiB), and more than a socket's buffers, so it's sent (and read) in pieces.
        std::string sent(std::size_t{1'024} * 1'024, 'x');
        for (std::size_t i = 0; i < sent.size(); ++i) sent[i] = static_cast<char>('a' + (i % 26));

        // NOTE: written while reading, as the server echoes it as it reads it: the write runs on the scheduler, and the
        //       read here, at once.
        const auto write = [](tls::stream* s, const std::string* sent) -> task<>
        { co_await net::io::write_all(*s, std::as_bytes(std::span{*sent})); };
        REQUIRE(sched->spawn(write(&*s, &sent)));

        std::string received;

        std::array<std::byte, 4'096> buf{};
        while (received.size() < sent.size())
        {
            auto res = co_await s->read(buf);
            received += as_string(std::span{buf}.first(res.count));
            if (res.err) co_return "read failed after " + std::to_string(received.size()) + ": " + res.err.message();
        }

        co_await s->close();
        co_return received == sent ? "ok" : "received something else";
    };

    CHECK(sched.run_to_completion(client(&sched, listener.local_port())) == "ok");
}

TEST_CASE("certificates that aren't the server's, or aren't trusted, are rejected", "[tls][verification]")
{
    struct test_case
    {
        std::string_view      name;
        std::filesystem::path chain;
        std::filesystem::path key;
        std::string           host;
        bool                  skip_verification;
        bool                  succeeds;
        std::string_view      reason = {};
    };

    // Botan's reasons.
    constexpr auto name_mismatch = "Certificate does not match provided name"sv;
    constexpr auto untrusted     = "Certificate issuer not found"sv;

    const auto& p  = pki();
    const auto  tc = GENERATE_REF(
        test_case{"another name's", p.other_name_chain, p.other_name_key, "localhost", false, false, name_mismatch},
        test_case{"an untrusted CA's", p.untrusted_chain, p.untrusted_key, "localhost", false, false, untrusted},
        test_case{"an untrusted CA's, not verified", p.untrusted_chain, p.untrusted_key, "localhost", true, true},
        test_case{"for the IP address", p.localhost_chain, p.localhost_key, "127.0.0.1", false, true},
        test_case{"not for the IP address",
                  p.other_name_chain,
                  p.other_name_key,
                  "127.0.0.1",
                  false,
                  false,
                  name_mismatch});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    server_outcome server{.sched = &sched, .err = {}};
    REQUIRE(sched.spawn(serve_once(&listener, server_with(tc.chain, tc.key), echo, &server)));

    auto ctx = tls::client_context::make({.trusted_ca_file = p.ca, .insecure_skip_verification = tc.skip_verification});
    REQUIRE(ctx.has_value());

    // the client's result, once the server's handshake has ended too.
    const auto client = [](net::io::scheduler* sched,
                           std::uint16_t       port,
                           tls::client_context ctx,
                           std::string         host,
                           server_outcome*     server) -> task<std::expected<tls::stream, std::error_code>>
    {
        auto s = co_await connect_to(sched, port, std::move(ctx), std::move(host));
        co_await server->wait();
        co_return s;
    };

    auto s = sched.run_to_completion(client(&sched, listener.local_port(), *ctx, tc.host, &server));
    CAPTURE(s.has_value() ? "connected" : s.error().message());
    CAPTURE(server.err.message());

    CHECK(s.has_value() == tc.succeeds);
    if (tc.succeeds) return;

    // the client says why it rejected the certificate, and the server that the client sent bad_certificate.
    CHECK(s.error().category() == tls::verification_category());
    CHECK(s.error() == tls::alert::bad_certificate);
    CHECK_FALSE(tls::from_peer(s.error()));
    CHECK(s.error().message().contains(tc.reason));

    CHECK(server.err == tls::alert::bad_certificate);
    CHECK(tls::from_peer(server.err));
    CHECK(server.err.message() == "bad_certificate (from the peer)");
}

TEST_CASE("a TLS stream ends with close_notify, and is cut short without it", "[tls]")
{
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    const bool notify = GENERATE(true, false);
    CAPTURE(notify);

    const auto send_then_end = [notify](tls::stream& s) -> task<>
    {
        co_await net::io::write_all(s, "the end"sv);
        if (notify) co_await s.close();

        // ends the stream without close_notify. NOTE: rather than closing the socket, which would reset the connection
        // if the client's last handshake message is still unread (as it may be, in TLS 1.3).
        else s.socket().shutdown_write();
    };
    REQUIRE(sched.spawn(serve_once(&listener, server_with(pki().localhost_chain, pki().localhost_key), send_then_end)));

    const auto client = [](net::io::scheduler* sched,
                           std::uint16_t       port) -> task<std::pair<std::string, std::error_code>>
    {
        auto s = co_await connect_to(sched, port, trusting_client(), "localhost");
        if (!s.has_value()) co_return std::pair{"connect failed: " + s.error().message(), s.error()};
        co_return co_await read_to_end(*s);
    };

    auto [received, err] = sched.run_to_completion(client(&sched, listener.local_port()));
    CAPTURE(err.message());
    CHECK(received == "the end");
    if (notify) CHECK(err == net::io::errc::end_of_stream);
    else CHECK(err == tls::errc::stream_truncated);
    CHECK(err == net::io::status_condition::closed);
}

TEST_CASE("shutting a stream down sends close_notify, and what the peer sent can still be read", "[tls]")
{
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // what the server read after shutting its side, and how its reading ended (which the client waits for).
    std::string    server_read;
    server_outcome server_done{.sched = &sched, .err = {}};

    const auto server = [](std::string* server_read, server_outcome* server_done)
    {
        return [server_read, server_done](tls::stream& s) -> task<>
        {
            co_await net::io::write_all(s, "ping"sv);
            if (auto err = co_await s.shutdown(); err)
            {
                server_done->finish(err);
                co_return;
            }

            auto [read, err] = co_await read_to_end(s);
            *server_read     = std::move(read);
            co_await s.close();
            server_done->finish(err);
        };
    };
    REQUIRE(sched.spawn(serve_once(&listener,
                                   server_with(pki().localhost_chain, pki().localhost_key),
                                   server(&server_read, &server_done))));

    const auto client = [](net::io::scheduler* sched,
                           std::uint16_t       port,
                           server_outcome*     server_done) -> task<std::pair<std::string, std::error_code>>
    {
        auto s = co_await connect_to(sched, port, trusting_client(), "localhost");
        if (!s.has_value()) co_return std::pair{"connect failed: " + s.error().message(), s.error()};

        // NOTE: sent before reading the server's close_notify, which is answered with the client's own (so nothing more
        //       could be written).
        co_await net::io::write_all(*s, "pong"sv);

        auto received = co_await read_to_end(*s);
        co_await server_done->wait();
        co_return received;
    };

    auto [received, err] = sched.run_to_completion(client(&sched, listener.local_port(), &server_done));

    CAPTURE(err.message());
    CHECK(received == "ping");
    CHECK(err == net::io::errc::end_of_stream);

    CAPTURE(server_done.err.message());
    CHECK(server_done.done.load() == 1);
    CHECK(server_read == "pong");
    CHECK(server_done.err == net::io::errc::end_of_stream);
}

TEST_CASE("requesting stop ends a handshake", "[tls]")
{
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    const bool already = GENERATE(true, false);
    CAPTURE(already);

    // a server that accepts the connection, but never answers.
    std::atomic<bool> done   = false;
    const auto        silent = [](net::listener* listener, std::atomic<bool>* done, net::io::scheduler* sched) -> task<>
    {
        auto accepted = co_await listener->accept();
        while (!done->load()) (void)co_await sched->sleep_until(net::io::clock::now() + 10ms);
    };
    REQUIRE(sched.spawn(silent(&listener, &done, &sched)));

    std::stop_source stop;
    if (already) stop.request_stop();

    const auto stop_soon = [](net::io::scheduler* sched, std::stop_source* stop) -> task<>
    {
        (void)co_await sched->sleep_until(net::io::clock::now() + 100ms);
        stop->request_stop();
    };
    if (!already) REQUIRE(sched.spawn(stop_soon(&sched, &stop)));

    const auto client = [](net::io::scheduler* sched,
                           std::uint16_t       port,
                           std::stop_token     token,
                           std::atomic<bool>*  done) -> task<std::error_code>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
        if (!conn.has_value()) co_return conn.error();

        auto s = co_await tls::stream::connect(std::move(*conn),
                                               trusting_client(),
                                               "localhost",
                                               net::io::clock::now() + 5s,
                                               std::move(token));
        done->store(true);
        co_return s.has_value() ? std::error_code{} : s.error();
    };

    const auto start = std::chrono::steady_clock::now();
    auto       err   = sched.run_to_completion(client(&sched, listener.local_port(), stop.get_token(), &done));
    CAPTURE(err.message());
    CHECK(err == std::errc::operation_canceled);
    CHECK(std::chrono::steady_clock::now() - start < 3s);
}

TEST_CASE("a handshake that doesn't finish by its deadline fails", "[tls]")
{
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // a server that accepts the connection, but never answers.
    std::atomic<bool> done = false;
    const auto        silent = [](net::listener* listener, std::atomic<bool>* done, net::io::scheduler* sched) -> task<>
    {
        auto accepted = co_await listener->accept();
        while (!done->load()) (void)co_await sched->sleep_until(net::io::clock::now() + 10ms);
    };
    REQUIRE(sched.spawn(silent(&listener, &done, &sched)));

    const auto client =
        [](net::io::scheduler* sched, std::uint16_t port, std::atomic<bool>* done) -> task<std::error_code>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
        if (!conn.has_value()) co_return conn.error();

        auto s = co_await tls::stream::connect(std::move(*conn), trusting_client(), "localhost", net::io::clock::now() + 200ms);
        done->store(true);
        co_return s.has_value() ? std::error_code{} : s.error();
    };

    const auto start = std::chrono::steady_clock::now();
    CHECK(sched.run_to_completion(client(&sched, listener.local_port(), &done)) == std::errc::timed_out);
    CHECK(std::chrono::steady_clock::now() - start < 3s);
}

TEST_CASE("contexts fail to load what isn't there", "[tls]")
{
    const auto missing = pki().dir / "missing.pem";

    auto client = tls::client_context::make({.trusted_ca_file = missing});
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error() == std::errc::no_such_file_or_directory);

    auto server = tls::server_context::make({.certificate_chain = missing, .private_key = pki().localhost_key});
    REQUIRE_FALSE(server.has_value());
    CHECK(server.error() == std::errc::no_such_file_or_directory);

    // a certificate isn't a key.
    auto not_a_key = tls::server_context::make({.certificate_chain = pki().localhost_chain, .private_key = pki().ca});
    CHECK_FALSE(not_a_key.has_value());
}

TEST_CASE("the system's trust store verifies a public server", "[tls][network]")
{
    // NOTE: letsencrypt.org's certificate has no revocation information (Let's Encrypt stopped OCSP), which mustn't be
    //       required. badssl.com speaks TLS 1.2 without the Extended Master Secret extension, which mustn't be either,
    //       unless asked for.
    const std::string host        = GENERATE("example.com", "letsencrypt.org", "badssl.com");
    const bool        require_ems = GENERATE(false, true);
    CAPTURE(host, require_ems);

    net::io::scheduler sched{make_pool()};

    const auto client = [](net::io::scheduler* sched, std::string host, bool require_ems) -> task<std::string>
    {
        auto ctx = tls::client_context::make({.require_extended_master_secret = require_ems});
        if (!ctx.has_value()) co_return "context: " + ctx.error().message();

        auto conn = co_await net::tcp_socket::connect(sched, host, "443");
        if (!conn.has_value()) co_return "connect: " + conn.error().message();

        auto s = co_await tls::stream::connect(std::move(*conn), *ctx, host, net::io::clock::now() + 10s);
        if (!s.has_value()) co_return "handshake: " + s.error().message();

        co_await s->close();
        co_return "ok";
    };

    const std::string_view expected =
        require_ems && host == "badssl.com" ? "handshake: handshake_failure (sent to the peer)" : "ok";
    CHECK(sched.run_to_completion(client(&sched, host, require_ems)) == expected);
}
