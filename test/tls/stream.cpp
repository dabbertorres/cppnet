#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "tls/certificate.hpp"
#include "tls/test_pki.hpp"
#include "tls/tls.hpp"
#include "tls/version.hpp"

namespace tls = net::tls;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::client_auth_server;
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

// say_hello connects to the listener over TLS (as localhost) and says hello, which the server echoes: returning what
// was echoed and the version agreed on (e.g. "hello over TLS 1.3"), or what failed.
task<std::string> say_hello(net::io::scheduler* sched, std::uint16_t port, tls::client_context ctx)
{
    auto s = co_await connect_to(sched, port, std::move(ctx), "localhost");
    if (!s.has_value()) co_return "handshake: " + s.error().message();

    if (auto res = co_await net::io::write_all(*s, "hello"sv); res.err) co_return "write: " + res.err.message();

    std::string               echoed;
    std::array<std::byte, 16> buf{};
    while (echoed.size() < 5)
    {
        auto res = co_await s->read(buf);
        echoed += as_string(std::span{buf}.first(res.count));
        if (res.err) co_return "read: " + res.err.message();
    }

    echoed += s->version() == tls::version::tls12 ? " over TLS 1.2" : " over TLS 1.3";
    co_await s->close();
    co_return echoed;
}

// handshakes records how a server's handshakes went (see recording_server()). It must outlive the server's contexts,
// and their streams.
struct handshakes
{
    std::mutex               mu;
    std::vector<std::string> done;

    [[nodiscard]] std::vector<std::string> taken()
    {
        std::scoped_lock lock{mu};
        return done;
    }
};

// described is how a handshake went: whether it resumed, the key exchange group agreed on (its code point; 0 over TLS
// 1.2), and whether the server sent a HelloRetryRequest, e.g. "full, 0x11ec" or "resumed, 0x11ec, after a
// HelloRetryRequest".
std::string described(const tls::detail::handshake_details& d)
{
    return std::format("{}, {:#x}{}",
                       d.resumed ? "resumed" : "full",
                       d.group,
                       d.hello_retry ? ", after a HelloRetryRequest" : "");
}

// recording_server is a server context with chain and key, recording how its handshakes went in record.
tls::server_context
recording_server(const std::filesystem::path& chain, const std::filesystem::path& key, handshakes* record)
{
    tls::detail::server_hooks hooks{.handshake_done = [record](const tls::detail::handshake_details& d)
                                    {
                                        std::scoped_lock lock{record->mu};
                                        record->done.push_back(described(d));
                                    }};

    auto ctx = tls::detail::make_server_context({.certificate_chain = chain, .private_key = key}, std::move(hooks));
    REQUIRE(ctx.has_value());
    return *ctx;
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

TEST_CASE("contexts refuse no TLS version as a minimum or maximum", "[tls][version]")
{
    struct test_case
    {
        tls::version min_version = tls::version::tls12;
        tls::version max_version = tls::version::tls13;
    };

    // none as both is in order, so it's refused as no version, not as out of order.
    const auto tc = GENERATE(test_case{.min_version = tls::version::none, .max_version = tls::version::tls13},
                             test_case{.min_version = tls::version::tls12, .max_version = tls::version::none},
                             test_case{.min_version = tls::version::none, .max_version = tls::version::none});
    CAPTURE(std::to_underlying(tc.min_version), std::to_underlying(tc.max_version));

    auto client = tls::client_context::make(
        {.trusted_ca_file = pki().ca, .min_version = tc.min_version, .max_version = tc.max_version});
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error() == std::errc::invalid_argument);

    auto server = tls::server_context::make({.certificate_chain = pki().localhost_chain,
                                             .private_key       = pki().localhost_key,
                                             .min_version       = tc.min_version,
                                             .max_version       = tc.max_version});
    REQUIRE_FALSE(server.has_value());
    CHECK(server.error() == std::errc::invalid_argument);
}

TEST_CASE("handshakes work with every key exchange group and cipher suite offered and either kind of certificate",
          "[tls][policy]")
{
    // The client offers one group (or cipher suite) of the policy's, so the handshake uses it: one that Botan's build
    // left out fails here, rather than a handshake in the field (see ports/botan). A group or suite the policy doesn't
    // offer fails too, as narrowing the policy to it leaves nothing.
    struct test_case
    {
        std::string_view           name{};
        tls::version               version = tls::version::tls13;
        std::vector<std::uint16_t> groups{};
        std::vector<std::uint16_t> cipher_suites{};
        bool                       rsa = false;
    };

    const auto tc = GENERATE(
        // TLS 1.3's groups (IANA's TLS Supported Groups).
        test_case{.name = "X25519", .groups = {29}},
        test_case{.name = "P-256", .groups = {23}},
        test_case{.name = "P-384", .groups = {24}},
        test_case{.name = "P-521", .groups = {25}},
        test_case{.name = "X25519MLKEM768", .groups = {0x11EC}},
        test_case{.name = "SecP256r1MLKEM768", .groups = {0x11EB}},
        test_case{.name = "SecP384r1MLKEM1024", .groups = {0x11ED}},
        // TLS 1.2's (ECDHE: the hybrids are TLS 1.3's only).
        test_case{.name = "X25519 (TLS 1.2)", .version = tls::version::tls12, .groups = {29}},
        test_case{.name = "P-256 (TLS 1.2)", .version = tls::version::tls12, .groups = {23}},
        test_case{.name = "P-384 (TLS 1.2)", .version = tls::version::tls12, .groups = {24}},
        test_case{.name = "P-521 (TLS 1.2)", .version = tls::version::tls12, .groups = {25}},
        // TLS 1.3's cipher suites (IANA's TLS Cipher Suites).
        test_case{.name = "TLS_AES_128_GCM_SHA256", .cipher_suites = {0x1301}},
        test_case{.name = "TLS_AES_256_GCM_SHA384", .cipher_suites = {0x1302}},
        test_case{.name = "TLS_CHACHA20_POLY1305_SHA256", .cipher_suites = {0x1303}},
        // TLS 1.2's, with the ECDSA certificate.
        test_case{.name          = "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256",
                  .version       = tls::version::tls12,
                  .cipher_suites = {0xC02B}},
        test_case{.name          = "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384",
                  .version       = tls::version::tls12,
                  .cipher_suites = {0xC02C}},
        test_case{.name          = "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256",
                  .version       = tls::version::tls12,
                  .cipher_suites = {0xCCA9}},
        // an RSA certificate (from an RSA CA): TLS 1.3's signatures are RSA-PSS; and TLS 1.2's suites for it.
        test_case{.name = "RSA certificate", .rsa = true},
        test_case{.name          = "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",
                  .version       = tls::version::tls12,
                  .cipher_suites = {0xC02F},
                  .rsa           = true},
        test_case{.name          = "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384",
                  .version       = tls::version::tls12,
                  .cipher_suites = {0xC030},
                  .rsa           = true},
        test_case{.name          = "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256",
                  .version       = tls::version::tls12,
                  .cipher_suites = {0xCCA8},
                  .rsa           = true});
    CAPTURE(tc.name);

    // NOTE: before the scheduler, as the server's handshake records into it.
    handshakes record;

    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    const auto server_ctx = tc.rsa ? recording_server(pki().rsa_localhost_chain, pki().rsa_localhost_key, &record)
                                   : recording_server(pki().localhost_chain, pki().localhost_key, &record);
    REQUIRE(sched.spawn(serve_once(&listener, server_ctx, echo)));

    auto client_ctx = tls::detail::make_client_context(
        {.trusted_ca_file = pki().ca, .min_version = tc.version, .max_version = tc.version},
        {.groups = tc.groups, .cipher_suites = tc.cipher_suites});
    REQUIRE(client_ctx.has_value());

    // the client says hello, which the server echoes, and the version agreed on.
    const auto expected = tc.version == tls::version::tls12 ? "hello over TLS 1.2"sv : "hello over TLS 1.3"sv;
    CHECK(sched.run_to_completion(say_hello(&sched, listener.local_port(), *client_ctx)) == expected);

    // TLS 1.3's group is the one the client was narrowed to (or the hybrid, by default), whose share it sent, so the
    // server didn't ask for another.
    std::uint16_t group = 0;
    if (tc.version == tls::version::tls13) group = tc.groups.empty() ? std::uint16_t{0x11EC} : tc.groups.front();
    CHECK(record.taken() == std::vector{described({.group = group})});
}

TEST_CASE("a client sends key shares for X25519MLKEM768 and X25519, so a server that prefers the hybrid needn't ask "
          "for its share",
          "[tls][policy]")
{
    // The client's first ClientHello has shares for the first hybrid group and the first elliptic curve one it offers
    // (of the policy's, or those it's narrowed to). Botan's server (so cppnet's) chooses a hybrid whenever the client
    // offers one, with a HelloRetryRequest if the client sent no share for it; otherwise a group the client sent a
    // share for. Each case connects twice, the second resuming the first's session, as resumptions exchange keys too.
    struct test_case
    {
        std::string_view           name{};
        std::vector<std::uint16_t> groups{};
        std::vector<std::uint16_t> key_shares{};
        std::uint16_t              group       = 0;
        bool                       hello_retry = false;
    };

    const auto tc = GENERATE(
        // X25519MLKEM768's share and X25519's.
        test_case{
            .name  = "the policy's groups",
            .group = 0x11EC
    },
        // a client without the hybrids (as an older one): X25519's share, and no HelloRetryRequest.
        test_case{.name = "X25519 and P-256", .groups = {29, 23}, .group = 29},
        test_case{.name = "P-256 and SecP256r1MLKEM768", .groups = {23, 0x11EB}, .group = 0x11EB},
        // no elliptic curve group: the first hybrid's share alone.
        test_case{.name = "the hybrids", .groups = {0x11EC, 0x11EB, 0x11ED}, .group = 0x11EC},
        test_case{.name = "P-384 and P-521", .groups = {24, 25}, .group = 24},
        // X25519's share alone, as Botan's default policy sends: the server asks for the hybrid's.
        test_case{.name = "X25519's share alone", .key_shares = {29}, .group = 0x11EC, .hello_retry = true});
    CAPTURE(tc.name);

    // NOTE: before the scheduler, as the server's handshakes record into it.
    handshakes record;

    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    const auto server_ctx = recording_server(pki().localhost_chain, pki().localhost_key, &record);

    auto client_ctx = tls::detail::make_client_context({.trusted_ca_file = pki().ca},
                                                       {.groups = tc.groups, .key_shares = tc.key_shares});
    REQUIRE(client_ctx.has_value());

    const auto test = [](net::io::scheduler* sched,
                         net::listener*      listener,
                         tls::server_context server_ctx,
                         tls::client_context client_ctx) -> task<std::vector<std::string>>
    {
        std::vector<std::string> out;
        for (int i = 0; i < 2; ++i)
        {
            if (!sched->spawn(serve_once(listener, server_ctx, echo))) co_return out;
            out.push_back(co_await say_hello(sched, listener->local_port(), client_ctx));
        }
        co_return out;
    };

    CHECK(sched.run_to_completion(test(&sched, &listener, server_ctx, *client_ctx))
          == std::vector<std::string>{"hello over TLS 1.3", "hello over TLS 1.3"});
    CHECK(record.taken()
          == std::vector{described({.group = tc.group, .hello_retry = tc.hello_retry}),
                         described({.resumed = true, .group = tc.group, .hello_retry = tc.hello_retry})});
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

TEST_CASE("a client's stream verifies the server's certificate for another host, as a handshake for it would",
          "[tls][verification][verify-for]")
{
    struct test_case
    {
        std::string_view                      name;
        std::string                           host;
        std::chrono::system_clock::duration   later; // verified this long from now
        std::string_view                      reason = {};
        std::chrono::system_clock::time_point at     = {};
        std::error_code                       err    = {};
    };

    // Botan's reasons (the certificate is valid from an hour ago for a day).
    constexpr auto name_mismatch = "Certificate does not match provided name"sv;
    constexpr auto expired       = "Certificate has expired"sv;
    constexpr auto not_yet_valid = "Certificate is not yet valid"sv;

    std::vector<test_case> cases{
        {.name = "its first name", .host = "a.test", .later = {}},
        {.name = "another of its names", .host = "b.test", .later = {}},
        {.name = "its names in any case", .host = "B.Test", .later = {}},
        {.name = "a wildcard's", .host = "x.w.test", .later = {}},
        {.name = "below a wildcard's", .host = "y.x.w.test", .later = {}, .reason = name_mismatch},
        {.name = "the wildcard's parent", .host = "w.test", .later = {}, .reason = name_mismatch},
        {.name = "a name it doesn't have", .host = "c.test", .later = {}, .reason = name_mismatch},
        {.name = "its IP address", .host = "127.0.0.1", .later = {}},
        {.name = "an IP address it doesn't have", .host = "127.0.0.2", .later = {}, .reason = name_mismatch},
        {.name = "expired since the handshake", .host = "b.test", .later = 48h, .reason = expired},
        {.name = "its own, expired since the handshake", .host = "a.test", .later = 48h, .reason = expired},
        {.name = "before it's valid", .host = "b.test", .later = -2h, .reason = not_yet_valid},
    };

    // the server's stream verifies nothing of the sort. NOTE: before the scheduler, whose tasks use them.
    bool           server_verifies = true;
    server_outcome server{.sched = nullptr, .err = {}};

    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);
    server.sched = &sched;

    const auto serve = [&server, &server_verifies](tls::stream& s) -> task<>
    {
        server_verifies = tls::detail::verifies(s);
        server.finish(tls::detail::verify_for(s, "a.test"));
        co_await echo(s);
    };
    REQUIRE(sched.spawn(serve_once(&listener, server_with(pki().names_chain, pki().names_key), serve)));

    struct client_side
    {
        std::error_code connected{};
        bool            verifies = false;
        std::error_code empty_host{};
    };
    const auto client = sched.run_to_completion(
        [](net::io::scheduler* sched, std::uint16_t port, std::vector<test_case>* cases, server_outcome* server)
            -> task<client_side>
        {
            auto s = co_await connect_to(sched, port, trusting_client(), "a.test");
            co_await server->wait();
            if (!s.has_value()) co_return client_side{.connected = s.error()};

            client_side out{.verifies = tls::detail::verifies(*s), .empty_host = tls::detail::verify_for(*s, "")};
            for (auto& tc : *cases)
            {
                const auto now = std::chrono::system_clock::now();
                tc.at          = now + tc.later;
                tc.err         = tc.later == decltype(tc.later){} ? tls::detail::verify_for(*s, tc.host)
                                                                  : tls::detail::verify_for(*s, tc.host, tc.at);
            }

            co_await s->close();
            co_return out;
        }(&sched, listener.local_port(), &cases, &server));

    CAPTURE(client.connected.message());
    REQUIRE_FALSE(client.connected);
    CHECK(client.verifies);
    CHECK(client.empty_host == std::errc::invalid_argument);
    REQUIRE(server.done.load() == 1);
    CHECK_FALSE(server_verifies);
    CHECK(server.err == std::errc::invalid_argument);

    for (const auto& tc : cases)
    {
        CAPTURE(tc.name, tc.host, tc.err.message());
        if (tc.reason.empty())
        {
            CHECK_FALSE(tc.err);
            continue;
        }

        // as the handshake says it: why, and bad_certificate.
        CHECK(tc.err.category() == tls::verification_category());
        CHECK(tc.err == tls::alert::bad_certificate);
        CHECK(tc.err.message().contains(tc.reason));
    }
}

TEST_CASE("a client's stream that doesn't verify the server's certificate can't verify it for another host",
          "[tls][verification][verify-for]")
{
    // with insecure_skip_verification, nothing says the server is anyone's: here, one an untrusted CA's certificate is
    // for, which a client that verifies refuses.
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);
    REQUIRE(sched.spawn(serve_once(&listener, server_with(pki().untrusted_chain, pki().untrusted_key), echo)));

    auto ctx = tls::client_context::make({.trusted_ca_file = pki().ca, .insecure_skip_verification = true});
    REQUIRE(ctx.has_value());

    const auto [connected, verifies, err] = sched.run_to_completion(
        [](net::io::scheduler* sched,
           std::uint16_t       port,
           tls::client_context ctx) -> task<std::tuple<std::error_code, bool, std::error_code>>
        {
            auto s = co_await connect_to(sched, port, std::move(ctx), "localhost");
            if (!s.has_value()) co_return std::tuple{s.error(), false, std::error_code{}};

            const auto out =
                std::tuple{std::error_code{}, tls::detail::verifies(*s), tls::detail::verify_for(*s, "localhost")};
            co_await s->close();
            co_return out;
        }(&sched, listener.local_port(), *ctx));

    CAPTURE(connected.message());
    REQUIRE_FALSE(connected);
    CHECK_FALSE(verifies);
    CHECK(err == std::errc::invalid_argument);
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

TEST_CASE("waiting for a stream to be writable waits for a write in progress, rather than alongside it",
          "[tls][wait-writable]")
{
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // the server reads everything, until close_notify; or reads nothing (until the client's done), while the write and
    // the wait are limited by a write timeout.
    const bool reads = GENERATE(true, false);
    CAPTURE(reads);

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

    // more than both sides' socket buffers hold, so that writing it waits for the peer to read.
    const std::string data(std::size_t{12} * 1'024 * 1'024, 'a');

    struct outcome
    {
        net::io::result write;
        net::io::result wait;
        std::error_code shutdown;
    };

    // await_write awaits a write on a task of its own, saying when it's about to (so that it's most likely flushing,
    // and waiting for the socket, by the time the client waits for it to be writable).
    const auto await_write =
        [](task<net::io::result> write, net::io::result* res, server_outcome* started, server_outcome* done) -> task<>
    {
        started->finish({});
        *res = co_await std::move(write);
        done->finish(res->err);
    };

    const auto client = [reads, await_write](net::io::scheduler* sched,
                                             std::uint16_t       port,
                                             const std::string*  data,
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
        if (!reads) s->socket().set_write_timeout(300ms);

        server_outcome started{.sched = sched, .err = {}};
        server_outcome written{.sched = sched, .err = {}};
        const bool     spawned =
            sched->spawn(await_write(s->write(std::as_bytes(std::span{*data})), &out.write, &started, &written));
        REQUIRE(spawned);
        co_await started.wait();

        out.wait = co_await s->wait_writable();
        co_await written.wait();

        if (reads) out.shutdown = co_await s->shutdown();
        client_done->finish({});
        co_await server_done->wait();
        co_await s->close();
        co_return out;
    };

    const auto out = sched.run_to_completion(client(&sched, listener.local_port(), &data, &server_done, &client_done));

    CAPTURE(out.write.err.message(), out.wait.err.message(), out.shutdown.message());
    CHECK(out.wait.count == 0);
    if (reads)
    {
        // the wait returned once the write had gone, and the socket could take more.
        CHECK_FALSE(out.write.err);
        CHECK(out.write.count == data.size());
        CHECK_FALSE(out.wait.err);
        CHECK_FALSE(out.shutdown);
        CHECK(server_read.size() == data.size());
    }
    else
    {
        // neither was refused for the other (device_or_resource_busy): the write timed out, and so did the wait,
        // with it.
        CHECK(out.write.err == std::errc::timed_out);
        CHECK(out.wait.err == std::errc::timed_out);
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

TEST_CASE("a server asks for clients' certificates as client_auth says, and verifies those sent", "[tls][client_auth]")
{
    // which certificate the client has (see test_pki).
    enum class has : std::uint8_t
    {
        none,
        good,
        expired,
        server_only,
        impostor, // issued by a CA with the client CA's name, so it's sent
        unnamed,  // issued by a CA the server doesn't name, so it isn't
    };

    // how the handshake ends.
    enum class outcome : std::uint8_t
    {
        with_certificate,    // the server has the client's certificate
        without_certificate, // the server has none
        none_sent,           // the server requires one, and the client sent none
        rejected,            // the client's certificate failed verification
    };

    struct test_case
    {
        std::string_view name;
        tls::client_auth auth;
        has              cert;
        outcome          expected;
        std::string_view reason = {}; // Botan's, why it was rejected
    };

    constexpr auto expired   = "Certificate has expired"sv;
    constexpr auto usage     = "Certificate does not allow the requested usage"sv;
    constexpr auto not_found = "Certificate issuer not found"sv;

    using enum tls::client_auth;
    const auto tc = GENERATE_REF(
        test_case{.name     = "none, without one",
                  .auth     = none,
                  .cert     = has::none,
                  .expected = outcome::without_certificate},
        test_case{.name     = "none, with one (never asked for)",
                  .auth     = none,
                  .cert     = has::good,
                  .expected = outcome::without_certificate},
        test_case{.name     = "optional, without one",
                  .auth     = optional,
                  .cert     = has::none,
                  .expected = outcome::without_certificate},
        test_case{.name     = "optional, with one",
                  .auth     = optional,
                  .cert     = has::good,
                  .expected = outcome::with_certificate},
        test_case{.name     = "optional, with an expired one",
                  .auth     = optional,
                  .cert     = has::expired,
                  .expected = outcome::rejected,
                  .reason   = expired},
        test_case{.name     = "optional, with one for servers",
                  .auth     = optional,
                  .cert     = has::server_only,
                  .expected = outcome::rejected,
                  .reason   = usage},
        test_case{.name     = "optional, with an impostor's",
                  .auth     = optional,
                  .cert     = has::impostor,
                  .expected = outcome::rejected,
                  .reason   = not_found},
        test_case{.name     = "optional, with one the server doesn't name",
                  .auth     = optional,
                  .cert     = has::unnamed,
                  .expected = outcome::without_certificate},
        test_case{.name = "required, without one", .auth = required, .cert = has::none, .expected = outcome::none_sent},
        test_case{.name     = "required, with one",
                  .auth     = required,
                  .cert     = has::good,
                  .expected = outcome::with_certificate},
        test_case{.name     = "required, with an expired one",
                  .auth     = required,
                  .cert     = has::expired,
                  .expected = outcome::rejected,
                  .reason   = expired},
        test_case{.name     = "required, with one for servers",
                  .auth     = required,
                  .cert     = has::server_only,
                  .expected = outcome::rejected,
                  .reason   = usage},
        test_case{.name     = "required, with an impostor's",
                  .auth     = required,
                  .cert     = has::impostor,
                  .expected = outcome::rejected,
                  .reason   = not_found},
        test_case{.name     = "required, with one the server doesn't name",
                  .auth     = required,
                  .cert     = has::unnamed,
                  .expected = outcome::none_sent});
    const auto version = GENERATE(tls::version::tls13, tls::version::tls12);
    CAPTURE(tc.name, version == tls::version::tls13 ? "TLS 1.3" : "TLS 1.2");

    const auto& p     = pki();
    const auto  files = [&p](has cert) -> std::pair<std::filesystem::path, std::filesystem::path>
    {
        switch (cert)
        {
        case has::none: return {};
        case has::good: return {p.client_chain, p.client_key};
        case has::expired: return {p.expired_client_chain, p.expired_client_key};
        case has::server_only: return {p.server_only_client_chain, p.server_only_client_key};
        case has::impostor: return {p.impostor_client_chain, p.impostor_client_key};
        case has::unnamed: return {p.unnamed_client_chain, p.unnamed_client_key};
        }
        return {};
    };
    const auto [chain, key] = files(tc.cert);

    server_outcome     server{.sched = nullptr, .err = {}};
    net::io::scheduler sched{make_pool()};
    server.sched = &sched;

    net::listener listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    // the server says how many certificates the client sent, and the first's subject, then closes.
    const auto say_certificates = [](tls::stream& s) -> task<>
    {
        const auto  certs = s.peer_certificates();
        std::string said  = std::to_string(certs->size());
        if (!certs->empty()) said += " " + certs->front().subject;

        co_await net::io::write_all(s, std::as_bytes(std::span{said}));
        co_await s.close();
    };
    REQUIRE(sched.spawn(serve_once(&listener, client_auth_server(tc.auth, version), say_certificates, &server)));

    struct result
    {
        std::error_code connect_err;
        std::string     said;
        std::error_code read_err;
    };

    // the client's result, once the server's handshake has ended too.
    const auto client = [](net::io::scheduler* sched,
                           std::uint16_t       port,
                           tls::client_context ctx,
                           server_outcome*     server) -> task<result>
    {
        result out;
        auto   s = co_await connect_to(sched, port, std::move(ctx), "localhost");
        if (s.has_value()) std::tie(out.said, out.read_err) = co_await read_to_end(*s);
        else out.connect_err = s.error();

        co_await server->wait();
        co_return out;
    };

    const auto out = sched.run_to_completion(
        client(&sched, listener.local_port(), test::client_with(chain, key, tls::version::tls13), &server));
    CAPTURE(out.connect_err.message(), out.said, out.read_err.message(), server.err.message());
    REQUIRE(server.done.load() == 1);

    switch (tc.expected)
    {
    case outcome::with_certificate:
    case outcome::without_certificate:
    {
        CHECK_FALSE(server.err);
        CHECK_FALSE(out.connect_err);
        CHECK(out.said == (tc.expected == outcome::with_certificate ? "1 " + p.client_subject : "0"));
        CHECK(out.read_err == net::io::errc::end_of_stream);
        break;
    }
    case outcome::none_sent:
    case outcome::rejected:
    {
        const auto alert = test::refusal(tc.expected == outcome::rejected, version);
        CHECK(server.err == alert);
        CHECK_FALSE(tls::from_peer(server.err));
        if (tc.expected == outcome::rejected)
        {
            CHECK(server.err.category() == tls::verification_category());
            CHECK(server.err.message().contains(tc.reason));
        }

        // the client's handshake is done, in TLS 1.3, before the server has looked at its certificate: so it's its
        // first read that fails. In TLS 1.2, the server's Finished comes after.
        const auto client_err = version == tls::version::tls13 ? out.read_err : out.connect_err;
        if (version == tls::version::tls13) CHECK_FALSE(out.connect_err);
        CHECK(out.said.empty());
        CHECK(client_err == alert);
        CHECK(tls::from_peer(client_err));
        break;
    }
    }
}

TEST_CASE("a peer's certificates are read from what it sent", "[tls][client_auth]")
{
    using certificates = std::shared_ptr<const std::vector<tls::certificate>>;

    // what the server saw of the client's certificates (asked for twice), and how its handshake ended.
    certificates       first;
    certificates       second;
    server_outcome     server{.sched = nullptr, .err = {}};
    net::io::scheduler sched{make_pool()};
    server.sched = &sched;

    net::listener listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    const auto keep = [](certificates* first, certificates* second)
    {
        return [first, second](tls::stream& s) -> task<>
        {
            *first  = s.peer_certificates();
            *second = s.peer_certificates();
            co_await s.close();
        };
    };
    REQUIRE(sched.spawn(
        serve_once(&listener, client_auth_server(tls::client_auth::required), keep(&first, &second), &server)));

    // the server's certificates, as the client saw them, once the server's handshake has ended too.
    const auto client = [](net::io::scheduler* sched, std::uint16_t port, server_outcome* server) -> task<certificates>
    {
        auto s = co_await connect_to(sched, port, test::client_with(pki().client_chain, pki().client_key), "localhost");
        if (!s.has_value()) co_return nullptr;

        auto certs = s->peer_certificates();
        (void)co_await read_to_end(*s);
        co_await server->wait();
        co_return certs;
    };

    const auto servers = sched.run_to_completion(client(&sched, listener.local_port(), &server));
    CAPTURE(server.err.message());

    const auto& p = pki();

    // the client's, as the server saw it: made once, then shared.
    REQUIRE(first != nullptr);
    CHECK(first == second);
    REQUIRE(first->size() == 1);

    const auto& alice = first->front();
    CHECK(alice.der == p.client_der);
    CHECK(alice.sha256 == p.client_sha256);
    CHECK(alice.subject == p.client_subject);
    CHECK(alice.issuer == "CN=cppnet test client CA");
    CHECK(alice.serial_number == p.client_serial);
    CHECK(alice.not_before == p.client_not_before);
    CHECK(alice.not_after == p.client_not_after);
    CHECK(alice.dns_names == std::vector<std::string>{"a.alice.example", "b.alice.example"});
    CHECK(alice.email_addresses == std::vector<std::string>{"alice@example.com"});
    CHECK(alice.uris == std::vector<std::string>{"spiffe://example.com/alice"});
    CHECK(alice.ip_addresses
          == std::vector<net::ip_addr>{
              net::ipv4_addr{192, 0, 2, 1},
              net::ip_addr::parse("2001:db8::1").value()
    });

    // the server's, as the client saw it.
    REQUIRE(servers != nullptr);
    REQUIRE(servers->size() == 1);

    const auto& localhost = servers->front();
    CHECK(localhost.subject == "CN=localhost");
    CHECK(localhost.issuer == "CN=cppnet test CA");
    CHECK(localhost.dns_names == std::vector<std::string>{"localhost"});
    CHECK(localhost.ip_addresses
          == std::vector<net::ip_addr>{
              net::ipv4_addr{127, 0, 0, 1}
    });
    CHECK(localhost.email_addresses.empty());
    CHECK(localhost.uris.empty());
    CHECK(localhost.not_before < std::chrono::system_clock::now());
    CHECK(localhost.not_after > std::chrono::system_clock::now());
    CHECK_FALSE(localhost.der.empty());
}

TEST_CASE("server contexts refuse client authentication without client CAs, or client CAs without it",
          "[tls][client_auth]")
{
    struct test_case
    {
        std::string_view                     name;
        tls::client_auth                     auth = tls::client_auth::required;
        std::optional<std::filesystem::path> client_ca{};
        std::error_code                      err{};
    };

    const auto& p       = pki();
    const auto  invalid = std::make_error_code(std::errc::invalid_argument);
    const auto  tc      = GENERATE_REF(
        test_case{.name = "none", .auth = tls::client_auth::none},
        test_case{.name = "optional", .auth = tls::client_auth::optional, .client_ca = p.client_ca},
        test_case{.name = "required", .client_ca = p.client_ca},
        test_case{.name = "optional without client CAs", .auth = tls::client_auth::optional, .err = invalid},
        test_case{.name = "required without client CAs", .err = invalid},
        test_case{.name      = "none with client CAs",
                  .auth      = tls::client_auth::none,
                  .client_ca = p.client_ca,
                  .err       = invalid},
        test_case{.name      = "not a mode",
                  .auth      = static_cast<tls::client_auth>(3),
                  .client_ca = p.client_ca,
                  .err       = invalid},
        test_case{.name      = "client CAs that aren't there",
                  .client_ca = p.dir / "missing.pem",
                  .err       = std::make_error_code(std::errc::no_such_file_or_directory)},
        test_case{.name = "client CAs that aren't certificates", .client_ca = p.client_key, .err = invalid});
    CAPTURE(tc.name);

    auto ctx = tls::server_context::make({.certificate_chain = p.localhost_chain,
                                          .private_key       = p.localhost_key,
                                          .client_auth       = tc.auth,
                                          .client_ca_file    = tc.client_ca});
    CHECK(ctx.has_value() == !tc.err);
    if (!ctx.has_value()) CHECK(ctx.error() == tc.err);
}

TEST_CASE("client contexts refuse a certificate without its key, or a key without its certificate, or another's",
          "[tls][client_auth]")
{
    struct test_case
    {
        std::string_view                     name;
        std::optional<std::filesystem::path> chain{};
        std::optional<std::filesystem::path> key{};
        std::error_code                      err{};
    };

    const auto& p       = pki();
    const auto  invalid = std::make_error_code(std::errc::invalid_argument);
    const auto  missing = std::make_error_code(std::errc::no_such_file_or_directory);
    const auto  tc      = GENERATE_REF(
        test_case{.name = "neither"},
        test_case{.name = "both", .chain = p.client_chain, .key = p.client_key},
        test_case{.name = "a certificate without its key", .chain = p.client_chain, .err = invalid},
        test_case{.name = "a key without its certificate", .key = p.client_key, .err = invalid},
        test_case{.name  = "another certificate's key",
                  .chain = p.client_chain,
                  .key   = p.expired_client_key,
                  .err   = invalid},
        test_case{.name  = "a certificate that isn't there",
                  .chain = p.dir / "missing.pem",
                  .key   = p.client_key,
                  .err   = missing},
        test_case{.name  = "a key that isn't there",
                  .chain = p.client_chain,
                  .key   = p.dir / "missing.key",
                  .err   = missing},
        test_case{.name = "a key that isn't a key", .chain = p.client_chain, .key = p.client_ca, .err = invalid});
    CAPTURE(tc.name);

    auto ctx =
        tls::client_context::make({.trusted_ca_file = p.ca, .certificate_chain = tc.chain, .private_key = tc.key});
    CHECK(ctx.has_value() == !tc.err);
    if (!ctx.has_value()) CHECK(ctx.error() == tc.err);
}

TEST_CASE("the system's trust store verifies a public server", "[tls][.network]")
{
    // NOTE: letsencrypt.org's certificate has no revocation information (Let's Encrypt stopped OCSP), which mustn't be
    //       required. badssl.com speaks TLS 1.2 without the Extended Master Secret extension, which mustn't be either,
    //       unless asked for.
    // NOTE: badssl.com is left out for now: from CI's runners it reset the connection (or timed out) about every other
    //       run, mostly the second handshake in a row. Its expected failure below stays, for when it's brought back (or
    //       a local server that likewise lacks the extension takes its place).
    const std::string host        = GENERATE("example.com", "letsencrypt.org" /*, "badssl.com" */);
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

TEST_CASE("public servers agree on the hybrid post-quantum key exchange", "[tls][policy][.network]")
{
    // X25519MLKEM768 against other implementations than Botan's (Cloudflare's and Google's offer it by default): the
    // client offers only it, so the handshake uses it.
    const std::string host = GENERATE("cloudflare.com", "www.google.com");
    CAPTURE(host);

    net::io::scheduler sched{make_pool()};

    const auto client = [](net::io::scheduler* sched, std::string host) -> task<std::string>
    {
        auto ctx = tls::detail::make_client_context({.min_version = tls::version::tls13}, {.groups = {0x11EC}});
        if (!ctx.has_value()) co_return "context: " + ctx.error().message();

        auto conn = co_await net::tcp_socket::connect(sched, host, "443");
        if (!conn.has_value()) co_return "connect: " + conn.error().message();

        auto s = co_await tls::stream::connect(std::move(*conn), *ctx, host, net::io::clock::now() + 10s);
        if (!s.has_value()) co_return "handshake: " + s.error().message();

        co_await s->close();
        co_return "ok";
    };

    CHECK(sched.run_to_completion(client(&sched, host)) == "ok");
}
