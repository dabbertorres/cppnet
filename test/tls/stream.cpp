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

std::string as_string(std::span<const std::byte> data)
{
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

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
        //       read here, at once. It's waited for (written) before the stream's closed and destroyed.
        server_outcome written{.sched = sched, .err = {}};
        const auto     write = [](tls::stream* s, const std::string* sent, server_outcome* written) -> task<>
        {
            auto res = co_await net::io::write_all(*s, std::as_bytes(std::span{*sent}));
            written->finish(res.err);
        };
        REQUIRE(sched->spawn(write(&*s, &sent, &written)));

        std::string received;

        std::array<std::byte, 4'096> buf{};
        while (received.size() < sent.size())
        {
            auto res = co_await s->read(buf);
            received += as_string(std::span{buf}.first(res.count));
            if (res.err) co_return "read failed after " + std::to_string(received.size()) + ": " + res.err.message();
        }

        co_await written.wait();
        if (written.done.load() == 0) co_return "the write didn't finish";
        if (written.err) co_return "write failed: " + written.err.message();

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
    std::atomic<bool> done   = false;
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

        auto s = co_await tls::stream::connect(std::move(*conn),
                                               trusting_client(),
                                               "localhost",
                                               net::io::clock::now() + 200ms);
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

TEST_CASE("a server context's key must be its certificate's", "[tls]")
{
    const auto& p = pki();

    auto matching = tls::server_context::make({.certificate_chain = p.localhost_chain, .private_key = p.localhost_key});
    CHECK(matching.has_value());

    auto mismatched =
        tls::server_context::make({.certificate_chain = p.localhost_chain, .private_key = p.other_name_key});
    REQUIRE_FALSE(mismatched.has_value());
    CHECK(mismatched.error() == std::errc::invalid_argument);
}

TEST_CASE("a TLS version is agreed on that both sides allow: the latest", "[tls][version]")
{
    struct test_case
    {
        std::string_view name{};
        tls::version     client_min = tls::version::tls12;
        tls::version     client_max = tls::version::tls13;
        tls::version     server_min = tls::version::tls12;
        tls::version     server_max = tls::version::tls13;
        tls::version     agreed     = tls::version::tls13;
    };

    const auto tc = GENERATE(test_case{.name = "both allow either"},
                             test_case{.name       = "the client allows TLS 1.2 only",
                                       .client_min = tls::version::tls12,
                                       .client_max = tls::version::tls12,
                                       .agreed     = tls::version::tls12},
                             test_case{.name       = "the server allows TLS 1.2 only",
                                       .server_min = tls::version::tls12,
                                       .server_max = tls::version::tls12,
                                       .agreed     = tls::version::tls12},
                             test_case{.name       = "the client allows TLS 1.3 only",
                                       .client_min = tls::version::tls13,
                                       .client_max = tls::version::tls13,
                                       .agreed     = tls::version::tls13},
                             test_case{.name       = "the server allows TLS 1.3 only",
                                       .server_min = tls::version::tls13,
                                       .server_max = tls::version::tls13,
                                       .agreed     = tls::version::tls13});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    auto server_ctx = tls::server_context::make({.certificate_chain = pki().localhost_chain,
                                                 .private_key       = pki().localhost_key,
                                                 .min_version       = tc.server_min,
                                                 .max_version       = tc.server_max});
    REQUIRE(server_ctx.has_value());

    auto client_ctx = tls::client_context::make(
        {.trusted_ca_file = pki().ca, .min_version = tc.client_min, .max_version = tc.client_max});
    REQUIRE(client_ctx.has_value());

    // the server says which version it agreed on, then closes.
    const auto say_version = [](tls::stream& s) -> task<>
    {
        co_await net::io::write_all(s, s.version() == tls::version::tls12 ? "1.2"sv : "1.3"sv);
        co_await s.close();
    };
    REQUIRE(sched.spawn(serve_once(&listener, *server_ctx, say_version)));

    const auto client = [](net::io::scheduler* sched,
                           std::uint16_t       port,
                           tls::client_context ctx) -> task<std::pair<std::string, std::string>>
    {
        auto s = co_await connect_to(sched, port, std::move(ctx), "localhost");
        if (!s.has_value()) co_return std::pair{"connect failed: " + s.error().message(), std::string{}};

        const std::string client_version = s->version() == tls::version::tls12 ? "1.2" : "1.3";
        auto [server_version, err]       = co_await read_to_end(*s);
        if (err != net::io::errc::end_of_stream) server_version += " then " + err.message();

        co_await s->close();
        co_return std::pair{client_version, server_version};
    };

    const auto [client_version, server_version] =
        sched.run_to_completion(client(&sched, listener.local_port(), *client_ctx));

    const auto expected = tc.agreed == tls::version::tls12 ? "1.2"sv : "1.3"sv;
    CHECK(client_version == expected);
    CHECK(server_version == expected);
}

TEST_CASE("a client and server that allow no TLS version in common fail the handshake", "[tls][version]")
{
    // either way, the server finds that the client offers nothing it allows, and sends protocol_version.
    const bool client_tls12 = GENERATE(true, false);
    CAPTURE(client_tls12);

    const auto client_version = client_tls12 ? tls::version::tls12 : tls::version::tls13;
    const auto server_version = client_tls12 ? tls::version::tls13 : tls::version::tls12;

    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    auto server_ctx = tls::server_context::make({.certificate_chain = pki().localhost_chain,
                                                 .private_key       = pki().localhost_key,
                                                 .min_version       = server_version,
                                                 .max_version       = server_version});
    REQUIRE(server_ctx.has_value());

    auto client_ctx = tls::client_context::make(
        {.trusted_ca_file = pki().ca, .min_version = client_version, .max_version = client_version});
    REQUIRE(client_ctx.has_value());

    server_outcome server{.sched = &sched, .err = {}};
    REQUIRE(sched.spawn(serve_once(&listener, *server_ctx, echo, &server)));

    // the client's result, once the server's handshake has ended too.
    const auto client = [](net::io::scheduler* sched,
                           std::uint16_t       port,
                           tls::client_context ctx,
                           server_outcome*     server) -> task<std::error_code>
    {
        auto s = co_await connect_to(sched, port, std::move(ctx), "localhost");
        co_await server->wait();
        co_return s.has_value() ? std::error_code{} : s.error();
    };

    const auto err = sched.run_to_completion(client(&sched, listener.local_port(), *client_ctx, &server));
    CAPTURE(err.message(), server.err.message());

    CHECK(err == tls::alert::protocol_version);
    CHECK(tls::from_peer(err));
    CHECK(err.message() == "protocol_version (from the peer)");

    CHECK(server.done.load() == 1);
    CHECK(server.err == tls::alert::protocol_version);
    CHECK_FALSE(tls::from_peer(server.err));
    CHECK(server.err.message() == "protocol_version (sent to the peer)");
}

TEST_CASE("contexts refuse a minimum TLS version after the maximum", "[tls][version]")
{
    auto client = tls::client_context::make(
        {.trusted_ca_file = pki().ca, .min_version = tls::version::tls13, .max_version = tls::version::tls12});
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error() == std::errc::invalid_argument);

    auto server = tls::server_context::make({.certificate_chain = pki().localhost_chain,
                                             .private_key       = pki().localhost_key,
                                             .min_version       = tls::version::tls13,
                                             .max_version       = tls::version::tls12});
    REQUIRE_FALSE(server.has_value());
    CHECK(server.error() == std::errc::invalid_argument);
}

TEST_CASE("a client needs the server's name to verify its certificate", "[tls][verification]")
{
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    const bool skip_verification = GENERATE(false, true);
    CAPTURE(skip_verification);

    // without verification, there's nothing to check the name against, so none is needed.
    server_outcome server{.sched = &sched, .err = {}};
    if (skip_verification)
        REQUIRE(
            sched.spawn(serve_once(&listener, server_with(pki().localhost_chain, pki().localhost_key), echo, &server)));

    auto ctx =
        tls::client_context::make({.trusted_ca_file = pki().ca, .insecure_skip_verification = skip_verification});
    REQUIRE(ctx.has_value());

    auto err = sched.run_to_completion(
        [](net::io::scheduler* sched, std::uint16_t port, tls::client_context ctx) -> task<std::error_code>
        {
            auto s = co_await connect_to(sched, port, std::move(ctx), "");
            if (!s.has_value()) co_return s.error();

            co_await s->close();
            co_return std::error_code{};
        }(&sched, listener.local_port(), *ctx));

    CAPTURE(err.message());
    if (skip_verification) CHECK_FALSE(err);
    else CHECK(err == std::errc::invalid_argument);
}

TEST_CASE("a read into an empty span returns at once", "[tls]")
{
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // sends nothing until the client has read into an empty span.
    server_outcome client_done{.sched = &sched, .err = {}};
    const auto     server = [](server_outcome* client_done)
    {
        return [client_done](tls::stream& s) -> task<>
        {
            co_await client_done->wait();
            co_await net::io::write_all(s, "hello"sv);
            co_await s.close();
        };
    };
    REQUIRE(sched.spawn(
        serve_once(&listener, server_with(pki().localhost_chain, pki().localhost_key), server(&client_done))));

    const auto client = [](net::io::scheduler* sched,
                           std::uint16_t       port,
                           server_outcome*     client_done) -> task<std::pair<net::io::result, std::string>>
    {
        auto s = co_await connect_to(sched, port, trusting_client(), "localhost");
        if (!s.has_value()) co_return std::pair{net::io::result{.err = s.error()}, std::string{}};

        // NOTE: a deadline, so a read that waits fails, rather than the test waiting for ever.
        s->socket().set_read_deadline(net::io::clock::now() + 5s);
        auto empty = co_await s->read(std::span<std::byte>{});
        client_done->finish({});

        // and the stream's still there to read.
        auto [received, _] = co_await read_to_end(*s);
        co_return std::pair{empty, received};
    };

    auto [empty, received] = sched.run_to_completion(client(&sched, listener.local_port(), &client_done));
    CAPTURE(empty.err.message());
    CHECK(empty.count == 0);
    CHECK_FALSE(empty.err);
    CHECK(received == "hello");
}

TEST_CASE("a stream knows whether it holds anything a read hasn't taken", "[tls]")
{
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // what the server sends after "hello" (in the same record, for data); then, but for data, the stream ends (without
    // close_notify, unless that's what was sent), which the client reads to, after "hello".
    enum class after : std::uint8_t
    {
        nothing,
        data,
        close_notify,
        part_of_a_header,
        part_of_a_record,
    };
    const auto then =
        GENERATE(after::nothing, after::data, after::close_notify, after::part_of_a_header, after::part_of_a_record);
    CAPTURE(static_cast<int>(then));

    server_outcome client_done{.sched = &sched, .err = {}};

    const auto server = [then](server_outcome* client_done)
    {
        return [then, client_done](tls::stream& s) -> task<>
        {
            switch (then)
            {
            case after::nothing:
                co_await net::io::write_all(s, "hello"sv);
                s.socket().shutdown_write();
                break;
            case after::data: co_await net::io::write_all(s, "hello, and more"sv); break;
            case after::close_notify:
                co_await net::io::write_all(s, "hello"sv);
                (void)co_await s.shutdown();
                break;
            case after::part_of_a_header:
            case after::part_of_a_record:
            {
                co_await net::io::write_all(s, "hello"sv);

                // a record's header, and some of its fragment: application data, 32 bytes long.
                const std::array<std::uint8_t, 5 + 8> part{0x17, 0x03, 0x03, 0x00, 0x20};
                const auto                            length = then == after::part_of_a_header ? 3 : part.size();
                co_await net::io::write_all(s.socket(), std::as_bytes(std::span{part}.first(length)));
                s.socket().shutdown_write();
                break;
            }
            }

            co_await client_done->wait();
            co_await s.close();
        };
    };

    REQUIRE(sched.spawn(
        serve_once(&listener, server_with(pki().localhost_chain, pki().localhost_key), server(&client_done))));

    struct outcome
    {
        std::string     received;
        std::error_code end;
        bool            clean = false;
    };

    const auto client =
        [then](net::io::scheduler* sched, std::uint16_t port, server_outcome* client_done) -> task<outcome>
    {
        outcome out;

        auto s = co_await connect_to(sched, port, trusting_client(), "localhost");
        if (!s.has_value())
        {
            out.end = s.error();
            co_return out;
        }

        std::array<std::byte, 5> buf{};
        std::size_t              got = 0;
        while (got < buf.size())
        {
            auto res = co_await s->read(std::span{buf}.subspan(got));
            got += res.count;
            if (res.err)
            {
                out.end = res.err;
                co_return out;
            }
        }
        out.received = as_string(buf);

        // the stream's end, after anything else the server sent: which the read takes (and holds) on the way.
        if (then != after::data) out.end = (co_await s->read(buf)).err;

        out.clean = tls::detail::idle_clean(*s);

        client_done->finish({});
        co_await s->close();
        co_return out;
    };

    auto out = sched.run_to_completion(client(&sched, listener.local_port(), &client_done));
    CAPTURE(out.end.message());
    CHECK(out.received == "hello");
    if (then == after::close_notify) CHECK(out.end == net::io::errc::end_of_stream);
    else if (then != after::data) CHECK(out.end == tls::errc::stream_truncated);
    CHECK(out.clean == (then == after::nothing));
}

TEST_CASE("a fatal alert ends a stream mid-way, on both sides", "[tls]")
{
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // sends "hello", then a record that isn't one (garbage that can't be decrypted), straight onto the socket; then
    // reads what the client says to that.
    server_outcome server_done{.sched = &sched, .err = {}};
    const auto     server = [](server_outcome* server_done)
    {
        return [server_done](tls::stream& s) -> task<>
        {
            co_await net::io::write_all(s, "hello"sv);

            // application data (23), TLS 1.2 (as TLS 1.3 says), 32 bytes long.
            std::array<std::uint8_t, 5 + 32> garbage{0x17, 0x03, 0x03, 0x00, 0x20};
            std::ranges::fill(std::span{garbage}.subspan(5), std::uint8_t{0xa5});
            co_await net::io::write_all(s.socket(), std::as_bytes(std::span{garbage}));

            auto [_, err] = co_await read_to_end(s);
            co_await s.close();
            server_done->finish(err);
        };
    };
    REQUIRE(sched.spawn(
        serve_once(&listener, server_with(pki().localhost_chain, pki().localhost_key), server(&server_done))));

    struct outcome
    {
        std::string     received;
        std::error_code read_err;
        std::error_code write_err;
    };

    const auto client = [](net::io::scheduler* sched, std::uint16_t port, server_outcome* server_done) -> task<outcome>
    {
        auto s = co_await connect_to(sched, port, trusting_client(), "localhost");
        if (!s.has_value()) co_return outcome{.received = {}, .read_err = s.error(), .write_err = {}};

        auto [received, read_err] = co_await read_to_end(*s);
        auto written              = co_await s->write(std::as_bytes(std::span{"more"sv}));
        co_await server_done->wait();
        co_return outcome{.received = received, .read_err = read_err, .write_err = written.err};
    };

    auto out = sched.run_to_completion(client(&sched, listener.local_port(), &server_done));

    // the client couldn't decrypt the record, so sent bad_record_mac, and failed: its reads and writes from then on.
    CAPTURE(out.read_err.message(), out.write_err.message());
    CHECK(out.received == "hello");
    CHECK(out.read_err == tls::alert::bad_record_mac);
    CHECK_FALSE(tls::from_peer(out.read_err));
    CHECK(out.write_err == out.read_err);

    // and the server read it.
    CAPTURE(server_done.err.message());
    CHECK(server_done.done.load() == 1);
    CHECK(server_done.err == tls::alert::bad_record_mac);
    CHECK(tls::from_peer(server_done.err));
}

TEST_CASE("writes return once their data has gone to a slow peer, or fail with it", "[tls]")
{
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // the server reads everything, until close_notify; or reads nothing (until the client's done), while the writes are
    // limited by a write timeout, or a write deadline.
    enum class peer : std::uint8_t
    {
        reads,
        stalls_timeout,
        stalls_deadline,
    };
    const auto mode = GENERATE(peer::reads, peer::stalls_timeout, peer::stalls_deadline);
    CAPTURE(static_cast<int>(mode));

    const bool reads    = mode == peer::reads;
    const bool deadline = mode == peer::stalls_deadline;

    std::string    server_read;
    server_outcome server_done{.sched = &sched, .err = {}};
    server_outcome client_done{.sched = &sched, .err = {}};

    const auto server = [reads](std::string* server_read, server_outcome* server_done, server_outcome* client_done)
    {
        return [reads, server_read, server_done, client_done](tls::stream& s) -> task<>
        {
            if (reads)
            {
                auto [read, err] = co_await read_to_end(s);
                *server_read     = std::move(read);
                server_done->finish(err);
            }
            else
            {
                co_await client_done->wait();
                server_done->finish({});
            }

            co_await s.close();
        };
    };
    REQUIRE(sched.spawn(serve_once(&listener,
                                   server_with(pki().localhost_chain, pki().localhost_key),
                                   server(&server_read, &server_done, &client_done))));

    // more than both sides' socket buffers hold, so that writing them waits for the peer to read.
    const std::string first(std::size_t{12} * 1'024 * 1'024, 'a');
    const std::string second(std::size_t{12} * 1'024 * 1'024, 'b');

    struct outcome
    {
        net::io::result first;
        net::io::result second;
        net::io::result later;
        std::error_code shutdown;
    };

    // await_write awaits a write, on a task of its own.
    const auto await_write = [](task<net::io::result> write, net::io::result* res, server_outcome* done) -> task<>
    {
        *res = co_await std::move(write);
        done->finish(res->err);
    };

    const auto client = [reads, deadline, await_write](net::io::scheduler* sched,
                                                       std::uint16_t       port,
                                                       const std::string*  first,
                                                       const std::string*  second,
                                                       server_outcome*     server_done,
                                                       server_outcome*     client_done) -> task<outcome>
    {
        outcome out;

        auto s = co_await connect_to(sched, port, trusting_client(), "localhost");
        if (!s.has_value())
        {
            out.shutdown = s.error();
            co_return out;
        }

        if (!reads && deadline) s->socket().set_write_deadline(net::io::clock::now() + 300ms);
        else if (!reads) s->socket().set_write_timeout(300ms);

        // both encrypted, in order, before either's task flushes: then two tasks flush them at once, the second to
        // start waiting for the first's (which writes both).
        auto           write_first  = s->write(std::as_bytes(std::span{*first}));
        auto           write_second = s->write(std::as_bytes(std::span{*second}));
        server_outcome first_done{.sched = sched, .err = {}};
        server_outcome second_done{.sched = sched, .err = {}};
        // NOTE: spawned outside REQUIRE, whose expression is in a loop (which moving from the tasks in can't be).
        const bool spawned_first  = sched->spawn(await_write(std::move(write_first), &out.first, &first_done));
        const bool spawned_second = sched->spawn(await_write(std::move(write_second), &out.second, &second_done));
        REQUIRE(spawned_first);
        REQUIRE(spawned_second);

        // close_notify goes after both.
        if (reads) out.shutdown = co_await s->shutdown();

        co_await first_done.wait();
        co_await second_done.wait();

        // a write that failed may have sent part of a record, so the next fails too (without a limit of its own).
        if (!reads)
        {
            s->socket().set_write_deadline(std::nullopt);
            s->socket().set_write_timeout(std::nullopt);
            out.later = co_await s->write(std::as_bytes(std::span{"more"sv}));
        }

        client_done->finish({});
        co_await server_done->wait();
        co_await s->close();
        co_return out;
    };

    auto out =
        sched.run_to_completion(client(&sched, listener.local_port(), &first, &second, &server_done, &client_done));

    CAPTURE(out.first.err.message(), out.second.err.message(), out.later.err.message(), out.shutdown.message());
    if (reads)
    {
        CHECK_FALSE(out.first.err);
        CHECK(out.first.count == first.size());
        CHECK_FALSE(out.second.err);
        CHECK(out.second.count == second.size());
        CHECK_FALSE(out.shutdown);

        // all of it, in order, then close_notify.
        CAPTURE(server_done.err.message());
        CHECK(server_done.err == net::io::errc::end_of_stream);
        CHECK(server_read.size() == first.size() + second.size());
        const bool in_order = server_read == first + second;
        CHECK(in_order);
    }
    else
    {
        // neither returned before its data had gone: the one waiting for the other's flush fails with it.
        CHECK(out.first.err == std::errc::timed_out);
        CHECK(out.second.err == std::errc::timed_out);
        CHECK(out.later.err == std::errc::timed_out);
    }
}

TEST_CASE("a read canceled by the stream's stop token leaves it to read again", "[tls][stop]")
{
    std::stop_source   source;
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // sends nothing until the client's read has been canceled.
    server_outcome canceled{.sched = &sched, .err = {}};
    const auto     server = [](server_outcome* canceled)
    {
        return [canceled](tls::stream& s) -> task<>
        {
            co_await canceled->wait();
            co_await net::io::write_all(s, "hello"sv);
            co_await s.close();
        };
    };
    REQUIRE(
        sched.spawn(serve_once(&listener, server_with(pki().localhost_chain, pki().localhost_key), server(&canceled))));

    const auto stop_soon = [](net::io::scheduler* sched, std::stop_source* source) -> task<>
    {
        (void)co_await sched->sleep_until(net::io::clock::now() + 50ms);
        source->request_stop();
    };

    const auto client = [stop_soon](net::io::scheduler* sched,
                                    std::uint16_t       port,
                                    std::stop_source*   source,
                                    server_outcome*     canceled) -> task<std::pair<net::io::result, std::string>>
    {
        auto s = co_await connect_to(sched, port, trusting_client(), "localhost");
        if (!s.has_value()) co_return std::pair{net::io::result{.err = s.error()}, std::string{}};

        s->set_stop_token(source->get_token());
        if (!sched->spawn(stop_soon(sched, source))) co_return std::pair{net::io::result{}, std::string{}};

        std::array<std::byte, 16> buf{};
        auto                      first = co_await s->read(buf);

        // NOTE: a deadline, so a read that waits fails, rather than the test waiting for ever.
        s->set_stop_token({});
        s->socket().set_read_deadline(net::io::clock::now() + 5s);
        canceled->finish({});

        auto [received, _] = co_await read_to_end(*s);
        co_return std::pair{first, received};
    };

    auto [first, received] = sched.run_to_completion(client(&sched, listener.local_port(), &source, &canceled));
    CAPTURE(first.err.message());
    CHECK(first.err == std::errc::operation_canceled);
    CHECK(first.count == 0);
    CHECK(received == "hello");
}

TEST_CASE("a write canceled by the stream's stop token fails the writes after it", "[tls][stop]")
{
    std::stop_source   source;
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // reads nothing until the client's done.
    server_outcome client_done{.sched = &sched, .err = {}};
    const auto     server = [](server_outcome* client_done)
    {
        return [client_done](tls::stream& s) -> task<>
        {
            co_await client_done->wait();
            co_await s.close();
        };
    };
    REQUIRE(sched.spawn(
        serve_once(&listener, server_with(pki().localhost_chain, pki().localhost_key), server(&client_done))));

    const auto stop_soon = [](net::io::scheduler* sched, std::stop_source* source) -> task<>
    {
        (void)co_await sched->sleep_until(net::io::clock::now() + 100ms);
        source->request_stop();
    };

    const auto client = [stop_soon](net::io::scheduler* sched,
                                    std::uint16_t       port,
                                    std::stop_source*   source,
                                    server_outcome* client_done) -> task<std::pair<net::io::result, net::io::result>>
    {
        auto s = co_await connect_to(sched, port, trusting_client(), "localhost");
        if (!s.has_value()) co_return std::pair{net::io::result{.err = s.error()}, net::io::result{}};

        s->set_stop_token(source->get_token());
        if (!sched->spawn(stop_soon(sched, source))) co_return std::pair{net::io::result{}, net::io::result{}};

        // more than both sides' socket buffers hold, so that writing it waits for the peer to read.
        const std::string lots(std::size_t{12} * 1'024 * 1'024, 'a');
        auto              first = co_await s->write(std::as_bytes(std::span{lots}));

        // part of a record may have gone, so nothing more can be written.
        s->set_stop_token({});
        auto later = co_await s->write(std::as_bytes(std::span{"more"sv}));

        client_done->finish({});
        co_await s->close();
        co_return std::pair{first, later};
    };

    auto [first, later] = sched.run_to_completion(client(&sched, listener.local_port(), &source, &client_done));
    CAPTURE(first.err.message(), later.err.message());
    CHECK(first.err == std::errc::operation_canceled);
    CHECK(later.err == std::errc::operation_canceled);
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
