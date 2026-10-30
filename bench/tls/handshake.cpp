// bench_tls compares full TLS 1.3 handshakes with resumed ones (session tickets: see tls::server_options), over
// loopback, with an ECDSA P-256 and an RSA 2048 server key, and with a client certificate (mutual TLS, ECDSA): one
// connection at a time, each connected, its handshake run, a byte read (which takes the server's ticket in too) and
// closed. "full, no tickets" is a server with session_tickets off; "full" one that makes a ticket for each connection,
// to clients that never resume (a new client context each); "resumed" every connection resuming the last's session.
//
// It reports, per connection, the time and the process's CPU time (both sides' work, and their scheduler's, as they
// share the process), in us, best of 3; and the same for a TCP connection alone, for what isn't TLS's. Nagle's
// algorithm is off on both sides (as the HTTP server turns it off), so the handshake's flights aren't held back.
//
// Results when this was written (macOS, Intel Core i9-9980HK laptop, Clang 23, -O3, Botan 3.12 trimmed as ports/botan
// builds it; us per connection, time / CPU time):
//
//   server key                 full, no tickets               full            resumed
//   ECDSA P-256                     1574 / 1551        1577 / 1554        1200 / 1166
//   RSA 2048                        3576 / 3511        3715 / 3650        1237 / 1205
//   ECDSA P-256, mutual TLS         2381 / 2341        2460 / 2413        1319 / 1296
//   TCP alone                          106 / 89
//
// (Under load, as when another build ran, all are up to half as much again.) Making a ticket costs a full handshake
// little (its AES-GCM and HMACs, against the signatures: within the runs' noise); resuming saves the certificates'
// work: about 0.4 ms with ECDSA, 1.1 ms with a client's certificate too, 2.5 ms with RSA. Half of what a resumption
// still costs is its key exchange's: cppnet's server prefers X25519MLKEM768 (with the client limited to X25519, by
// tls::detail::make_client_context()'s limits, a resumption took 633 / 617 us, and a full ECDSA handshake 1260 / 1231).
// When this was written the client sent a key share for X25519 alone, so the server asked for the hybrid's with a
// HelloRetryRequest; it now sends both (as Go and Chrome do), which saves that round trip: within these runs' noise
// over loopback (where a round trip costs little, and the client made both shares before too), but a network's round
// trip per handshake, resumed or not. The CPU time includes the pool's threads spinning as they wait for each other.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <random>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <botan/asn1_obj.h>
#include <botan/ec_group.h>
#include <botan/ecdsa.h>
#include <botan/pk_keys.h>
#include <botan/pkcs8.h>
#include <botan/rsa.h>
#include <botan/system_rng.h>
#include <botan/x509_ca.h>
#include <botan/x509cert.h>
#include <botan/x509self.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "tls/tls.hpp"
#include "tls/version.hpp"

namespace
{

using net::coro::task;
using namespace std::chrono_literals;

constexpr int connections = 200;

void write_file(const std::filesystem::path& path, std::string_view content)
{
    std::ofstream out{path};
    out << content;
}

// pki is a CA, and a certificate from it (for localhost) with its key, as PEM files.
struct pki
{
    std::filesystem::path ca;
    std::filesystem::path chain;
    std::filesystem::path key;
};

// make_pki makes a CA and a certificate for name (a server's, or a client's if client), with RSA keys if rsa
// (otherwise ECDSA's, on P-256), in dir.
pki make_pki(const std::filesystem::path& dir, const std::string& name, bool rsa, bool client = false)
{
    Botan::System_RNG rng;
    const auto        make_key = [&] -> std::unique_ptr<Botan::Private_Key>
    {
        if (rsa) return std::make_unique<Botan::RSA_PrivateKey>(rng, 2048);
        return std::make_unique<Botan::ECDSA_PrivateKey>(rng, Botan::EC_Group::from_name("secp256r1"));
    };

    const auto               ca_key = make_key();
    Botan::X509_Cert_Options ca_opts;
    ca_opts.common_name = name + " CA";
    ca_opts.CA_key();
    const auto ca = Botan::X509::create_self_signed_cert(ca_opts, *ca_key, "SHA-256", rng);

    const auto               key = make_key();
    Botan::X509_Cert_Options opts;
    opts.common_name = "localhost";
    opts.dns         = "localhost";
    opts.add_ex_constraint(client ? "PKIX.ClientAuth" : "PKIX.ServerAuth");
    const auto           request = Botan::X509::create_cert_req(opts, *key, "SHA-256", rng);
    const Botan::X509_CA authority{ca, *ca_key, "SHA-256", rng};
    const auto           now = std::chrono::system_clock::now();
    const auto cert = authority.sign_request(request, rng, Botan::X509_Time{now - 1h}, Botan::X509_Time{now + 24h});

    pki out{.ca = dir / (name + "-ca.pem"), .chain = dir / (name + ".pem"), .key = dir / (name + ".key")};
    write_file(out.ca, ca.PEM_encode());
    write_file(out.chain, cert.PEM_encode());
    write_file(out.key, Botan::PKCS8::PEM_encode(*key));
    return out;
}

// nodelay turns Nagle's algorithm off for conn (saying so if it can't).
void nodelay(const net::tcp_socket& conn)
{
    if (auto set = conn.set_nodelay(); !set.has_value())
        std::println(stderr, "failed to set TCP_NODELAY: {}", set.error().message());
}

// serve serves connections until accepting fails (the scheduler stops): each one's handshake, then a byte, then
// close_notify.
task<> serve(net::listener* listener, net::tls::server_context ctx)
{
    while (true)
    {
        auto accepted = co_await listener->accept();
        if (!accepted.has_value()) co_return;
        nodelay(*accepted);

        auto s = co_await net::tls::stream::accept(std::move(*accepted), ctx, net::io::clock::now() + 5s);
        if (!s.has_value())
        {
            std::println(stderr, "the server's handshake failed: {}", s.error().message());
            continue;
        }

        (void)co_await net::io::write_all(*s, std::string_view{"x"});
        co_await s->close();
    }
}

// connect_once connects, runs the client's handshake, reads the server's byte, and closes, returning whether the
// handshake resumed a session.
task<std::expected<bool, std::error_code>>
connect_once(net::io::scheduler* sched, std::uint16_t port, net::tls::client_context ctx)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    if (!conn.has_value()) co_return std::unexpected(conn.error());
    nodelay(*conn);

    auto s = co_await net::tls::stream::connect(std::move(*conn), ctx, "localhost", net::io::clock::now() + 5s);
    if (!s.has_value()) co_return std::unexpected(s.error());

    std::array<std::byte, 1> byte{};
    const auto               res = co_await s->read(byte);
    if (res.count != 1) co_return std::unexpected(res.err ? res.err : std::make_error_code(std::errc::bad_message));

    co_await s->close();
    co_return s->resumed();
}

// cost is what a connection took: the time, and the process's CPU time (both sides', and their scheduler's), in us.
struct cost
{
    double wall = -1;
    double cpu  = -1;
};

// meter measures connections' cost from when it's made.
struct meter
{
    std::chrono::steady_clock::time_point wall = std::chrono::steady_clock::now();
    std::clock_t                          cpu  = std::clock();

    [[nodiscard]] cost per_connection() const
    {
        const auto elapsed = std::chrono::steady_clock::now() - wall;
        return {
            .wall = std::chrono::duration<double, std::micro>(elapsed).count() / connections,
            .cpu  = static_cast<double>(std::clock() - cpu) * 1e6 / CLOCKS_PER_SEC / connections,
        };
    }
};

// best is the least of a and b, each.
cost best(cost a, cost b)
{
    if (a.wall < 0) return b;
    return {.wall = std::min(a.wall, b.wall), .cpu = std::min(a.cpu, b.cpu)};
}

// run makes connections, each with the next of clients (the same one, if there's only one), returning their cost, or
// none (-1) if one failed, or resumed other than as expected.
task<cost>
run(net::io::scheduler* sched, std::uint16_t port, const std::vector<net::tls::client_context>* clients, bool resumes)
{
    // NOTE: the first connection of a client that resumes makes its first ticket.
    if (resumes && !(co_await connect_once(sched, port, clients->front())).has_value()) co_return cost{};

    const meter m;
    for (std::size_t i = 0; i < connections; ++i)
    {
        const auto resumed = co_await connect_once(sched, port, (*clients)[i % clients->size()]);
        if (!resumed.has_value())
        {
            std::println(stderr, "a connection failed: {}", resumed.error().message());
            co_return cost{};
        }
        if (*resumed != resumes)
        {
            std::println(stderr, "a connection {} when it shouldn't have", *resumed ? "resumed" : "didn't resume");
            co_return cost{};
        }
    }
    co_return m.per_connection();
}

// serve_tcp serves connections as serve() does, without TLS.
task<> serve_tcp(net::listener* listener)
{
    while (true)
    {
        auto accepted = co_await listener->accept();
        if (!accepted.has_value()) co_return;

        (void)co_await net::io::write_all(*accepted, std::string_view{"x"});
        accepted->close();
    }
}

// run_tcp is run() without TLS: connections, each reading the server's byte.
task<cost> run_tcp(net::io::scheduler* sched, std::uint16_t port)
{
    const meter m;
    for (std::size_t i = 0; i < connections; ++i)
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
        if (!conn.has_value()) co_return cost{};
        nodelay(*conn);

        std::array<std::byte, 1> byte{};
        if ((co_await conn->read(byte)).count != 1) co_return cost{};
        conn->close();
    }
    co_return m.per_connection();
}

// measure_tcp is a connection's cost without TLS (best of 3): what a connection costs besides its handshake.
cost measure_tcp()
{
    cost out;
    for (int round = 0; round < 3; ++round)
    {
        net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
        auto               listener = net::listener::make(&sched, "127.0.0.1", "0");
        if (!listener.has_value() || !sched.spawn(serve_tcp(&*listener))) return {};

        const auto c = sched.run_to_completion(run_tcp(&sched, listener->local_port()));
        if (c.wall < 0) return {};
        out = best(out, c);
    }
    return out;
}

// mode is how the connections are made.
enum class mode : std::uint8_t
{
    full_without_tickets,
    full,
    resumed,
};

// measure is a connection's cost to a server with options, by clients made with client (best of 3).
cost measure(net::tls::server_options options, const net::tls::client_options& client, mode m)
{
    options.session_tickets = m != mode::full_without_tickets;
    auto server             = net::tls::server_context::make(options);
    if (!server.has_value())
    {
        std::println(stderr, "the server's context: {}", server.error().message());
        return {};
    }

    cost out;
    for (int round = 0; round < 3; ++round)
    {
        // a client for each connection that doesn't resume (so it has no ticket to offer), made beforehand.
        std::vector<net::tls::client_context> clients;
        const std::size_t                     count = m == mode::resumed ? 1 : connections;
        for (std::size_t i = 0; i < count; ++i)
        {
            auto ctx = net::tls::client_context::make(client);
            if (!ctx.has_value())
            {
                std::println(stderr, "the client's context: {}", ctx.error().message());
                return {};
            }
            clients.push_back(std::move(*ctx));
        }

        net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
        auto               listener = net::listener::make(&sched, "127.0.0.1", "0");
        if (!listener.has_value())
        {
            std::println(stderr, "listening: {}", listener.error().message());
            return {};
        }
        if (!sched.spawn(serve(&*listener, *server))) return {};

        const auto c = sched.run_to_completion(run(&sched, listener->local_port(), &clients, m == mode::resumed));
        if (c.wall < 0) return {};
        out = best(out, c);
    }
    return out;
}

}

int main()
{
    const auto dir =
        std::filesystem::temp_directory_path() / ("cppnet-bench-tls-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);

    const auto ecdsa  = make_pki(dir, "ecdsa", false);
    const auto rsa    = make_pki(dir, "rsa", true);
    const auto client = make_pki(dir, "client", false, true);

    struct setup
    {
        std::string_view         name;
        net::tls::server_options server;
        net::tls::client_options client;
    };

    const std::array setups{
        setup{
              .name   = "ECDSA P-256",
              .server = {.certificate_chain = ecdsa.chain, .private_key = ecdsa.key},
              .client = {.trusted_ca_file = ecdsa.ca, .min_version = net::tls::version::tls13},
              },
        setup{
              .name   = "RSA 2048",
              .server = {.certificate_chain = rsa.chain, .private_key = rsa.key},
              .client = {.trusted_ca_file = rsa.ca, .min_version = net::tls::version::tls13},
              },
        setup{
              .name   = "ECDSA P-256, mutual TLS",
              .server = {.certificate_chain = ecdsa.chain,
              .private_key       = ecdsa.key,
              .client_auth       = net::tls::client_auth::required,
              .client_ca_file    = client.ca},
              .client = {.trusted_ca_file   = ecdsa.ca,
              .min_version       = net::tls::version::tls13,
              .certificate_chain = client.chain,
              .private_key       = client.key},
              },
    };

    // NOTE: each "time / CPU time", in us.
    const auto show = [](cost c) { return std::format("{:.0f} / {:.0f}", c.wall, c.cpu); };

    std::println("{:<24} {:>18} {:>18} {:>18}", "server key", "full, no tickets", "full", "resumed");
    for (const auto& s : setups)
    {
        const auto without = measure(s.server, s.client, mode::full_without_tickets);
        const auto full    = measure(s.server, s.client, mode::full);
        const auto resumed = measure(s.server, s.client, mode::resumed);
        std::println("{:<24} {:>18} {:>18} {:>18}", s.name, show(without), show(full), show(resumed));
    }

    std::println("{:<24} {:>18}", "TCP alone", show(measure_tcp()));

    std::filesystem::remove_all(dir);
}
