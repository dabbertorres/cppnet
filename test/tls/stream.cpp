#include "tls/tls.hpp"

// NOTE: before Botan's headers, some of which use <algorithm> without including it (see ports/botan).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <botan/ec_group.h>
#include <botan/ecdsa.h>
#include <botan/pkcs10.h>
#include <botan/pkcs8.h>
#include <botan/system_rng.h>
#include <botan/x509_ca.h>
#include <botan/x509cert.h>
#include <botan/x509self.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace tls = net::tls;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;

namespace
{

// test_pki is certificate authorities, and certificates they issue, as PEM files in a temporary directory: made when the
// tests run, so that none expire.
struct test_pki
{
    std::filesystem::path dir;

    // the CA the tests trust.
    std::filesystem::path ca;

    // a certificate (and key) for localhost and 127.0.0.1, from the CA.
    std::filesystem::path localhost_chain;
    std::filesystem::path localhost_key;

    // one for another name, from the CA.
    std::filesystem::path other_name_chain;
    std::filesystem::path other_name_key;

    // one for localhost, from a CA nothing trusts.
    std::filesystem::path untrusted_chain;
    std::filesystem::path untrusted_key;
};

void write_file(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream out{path};
    out << content;
}

const test_pki& pki()
{
    static const test_pki made = []
    {
        Botan::System_RNG rng;
        const auto        group = Botan::EC_Group::from_name("secp256r1");

        test_pki out;
        out.dir = std::filesystem::temp_directory_path() / ("cppnet-tls-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(out.dir);

        const auto make_ca = [&](const std::string& name)
        {
            auto                     key = std::make_unique<Botan::ECDSA_PrivateKey>(rng, group);
            Botan::X509_Cert_Options opts;
            opts.common_name = name;
            opts.CA_key();
            auto cert = Botan::X509::create_self_signed_cert(opts, *key, "SHA-256", rng);
            return std::pair{cert, std::move(key)};
        };

        // issue writes a certificate for dns (and ip, if any), from ca, and its key.
        const auto issue = [&](const auto&                  ca,
                               const std::string&           dns,
                               const std::string&           ip,
                               const std::filesystem::path& chain,
                               const std::filesystem::path& key_path)
        {
            const Botan::ECDSA_PrivateKey key{rng, group};
            Botan::X509_Cert_Options      opts;
            opts.common_name = dns;
            opts.dns         = dns;
            opts.ip          = ip;

            const auto           request = Botan::X509::create_cert_req(opts, key, "SHA-256", rng);
            const Botan::X509_CA authority{ca.first, *ca.second, "SHA-256", rng};

            const auto now  = std::chrono::system_clock::now();
            const auto cert = authority.sign_request(request, rng, Botan::X509_Time{now - 1h}, Botan::X509_Time{now + 24h});

            write_file(chain, cert.PEM_encode());
            write_file(key_path, Botan::PKCS8::PEM_encode(key));
        };

        const auto trusted   = make_ca("cppnet test CA");
        const auto untrusted = make_ca("cppnet untrusted CA");

        out.ca = out.dir / "ca.pem";
        write_file(out.ca, trusted.first.PEM_encode());

        out.localhost_chain  = out.dir / "localhost.pem";
        out.localhost_key    = out.dir / "localhost.key";
        out.other_name_chain = out.dir / "other.pem";
        out.other_name_key   = out.dir / "other.key";
        out.untrusted_chain  = out.dir / "untrusted.pem";
        out.untrusted_key    = out.dir / "untrusted.key";

        issue(trusted, "localhost", "127.0.0.1", out.localhost_chain, out.localhost_key);
        issue(trusted, "other.example", "", out.other_name_chain, out.other_name_key);
        issue(untrusted, "localhost", "127.0.0.1", out.untrusted_chain, out.untrusted_key);
        return out;
    }();

    return made;
}

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

tls::client_context trusting_client(std::vector<std::string> alpn = {})
{
    auto ctx = tls::client_context::make({.trusted_ca_file = pki().ca, .alpn = std::move(alpn)});
    REQUIRE(ctx.has_value());
    return *ctx;
}

tls::server_context server_with(const std::filesystem::path& chain, const std::filesystem::path& key)
{
    auto ctx = tls::server_context::make({.certificate_chain = chain, .private_key = key});
    REQUIRE(ctx.has_value());
    return *ctx;
}

std::string as_string(std::span<const std::byte> data) { return {reinterpret_cast<const char*>(data.data()), data.size()}; }

// read_to_end reads s until it ends (or fails), returning what it read and why it stopped.
task<std::pair<std::string, std::error_condition>> read_to_end(net::io::reader& s)
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

// serve_once accepts a connection, runs the server's handshake, and hands the stream to serve (or the handshake's error
// to failed, if any).
template<typename Serve>
task<> serve_once(net::listener* listener, tls::server_context ctx, Serve serve, std::error_condition* failed = nullptr)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value()) co_return;

    auto s = co_await tls::stream::accept(std::move(*accepted), ctx, net::io::clock::now() + 5s);
    if (!s.has_value())
    {
        if (failed != nullptr) *failed = s.error();
        co_return;
    }

    co_await serve(*s);
}

// connect_to connects to the listener over TLS, as host.
task<std::expected<tls::stream, std::error_condition>>
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

std::error_condition alert(int code) { return {code, tls::category()}; }

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
    };

    const auto& p  = pki();
    const auto  tc = GENERATE_REF(
        test_case{"another name's", p.other_name_chain, p.other_name_key, "localhost", false, false},
        test_case{"an untrusted CA's", p.untrusted_chain, p.untrusted_key, "localhost", false, false},
        test_case{"an untrusted CA's, not verified", p.untrusted_chain, p.untrusted_key, "localhost", true, true},
        test_case{"for the IP address", p.localhost_chain, p.localhost_key, "127.0.0.1", false, true},
        test_case{"not for the IP address", p.other_name_chain, p.other_name_key, "127.0.0.1", false, false});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    std::error_condition server_failed;
    REQUIRE(sched.spawn(serve_once(&listener, server_with(tc.chain, tc.key), echo, &server_failed)));

    auto ctx = tls::client_context::make({.trusted_ca_file = p.ca, .insecure_skip_verification = tc.skip_verification});
    REQUIRE(ctx.has_value());

    auto s = sched.run_to_completion(connect_to(&sched, listener.local_port(), *ctx, tc.host));
    CAPTURE(s.has_value() ? "connected" : s.error().message());

    CHECK(s.has_value() == tc.succeeds);
    if (!tc.succeeds) CHECK(s.error() == alert(42)); // bad_certificate
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

    const auto client = [](net::io::scheduler* sched, std::uint16_t port) -> task<std::pair<std::string, std::error_condition>>
    {
        auto s = co_await connect_to(sched, port, trusting_client(), "localhost");
        if (!s.has_value()) co_return std::pair{"connect failed: " + s.error().message(), s.error()};
        co_return co_await read_to_end(*s);
    };

    auto [received, err] = sched.run_to_completion(client(&sched, listener.local_port()));
    CAPTURE(err.message());
    CHECK(received == "the end");
    if (notify) CHECK(err == net::io::status_condition::closed);
    else CHECK(err == tls::errc::stream_truncated);
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

    const auto client = [](net::io::scheduler* sched, std::uint16_t port, std::atomic<bool>* done) -> task<std::error_condition>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
        if (!conn.has_value()) co_return conn.error();

        auto s = co_await tls::stream::connect(std::move(*conn), trusting_client(), "localhost", net::io::clock::now() + 200ms);
        done->store(true);
        co_return s.has_value() ? std::error_condition{} : s.error();
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
    //       required.
    const std::string host = GENERATE("example.com", "letsencrypt.org");
    CAPTURE(host);

    net::io::scheduler sched{make_pool()};

    const auto client = [](net::io::scheduler* sched, std::string host) -> task<std::string>
    {
        auto ctx = tls::client_context::make();
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
