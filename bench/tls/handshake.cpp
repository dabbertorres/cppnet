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
//
// Then large writes (alone with "writes" as its argument): over one connection, the server writes 256 MiB in writes of
// 16 KiB to 16 MiB, which the client reads; per write, the time, the allocations (both sides'), and the most the heap
// held meanwhile through operator new (over what it held before: Botan's own secure allocations aren't counted). A
// write over 64 KiB is encrypted and sent a batch at a time, so the peak stays about 360 KiB whatever its size (a
// 16 MiB write's was 48 MiB when it was encrypted whole); about 420 MiB/s, the encryption's pace, for every size.
//
// Not run by default, the TLS context's first load: with "context" as its argument, then what to measure, each run in a
// process of its own (so the system's trust store is read for the first time, as a program's first https request
// does):
//
// - "make": client_context::make() with the system's trust store, then again. "make-insecure" does the same with
//   insecure_skip_verification, which needs no store.
// - "verify": a handshake with a context trusting the server's CA (a file), for what isn't the store's, then two with
//   the system's store, which doesn't trust it (bad_certificate), so they verify against it: the first in the process,
//   and the next. "verify-warm" does the same with the store's first lookup before the handshakes
//   (tls::detail::warm_up(), as http::client's pool has it).
// - "second": a context with the system's store, warmed up, then a handshake verifying against it; then a second
//   context's make(), warm-up and first handshake: what another context (e.g. http::client's for HTTP/1.1 alone, which
//   it makes beside its HTTP/2 one, or another client's) costs once the first has read the store.
// - "ticker <workers>": on a pool of that many workers, a task ticking every millisecond while an http::client (the
//   system's trust store, as by default) makes its first https request, to a local server whose certificate it doesn't
//   trust: how late the ticks were at worst meanwhile (and over 300 ms before it, idle), and how long the request took.
//   Whatever blocks a worker (making the context there) holds up the tasks waiting for it.
//
// Results when this was written (the same laptop, -O3; medians of 5 processes each, ms): make() 3.9 the first time,
// 0.1 the next; a handshake verifying against the system's store 10.0 the first time, 1.5 the next (2.0 with a CA
// file), and after warm_up() (8.3) 1.4: so on macOS most of the store's first-time cost is its first lookup
// (Security.framework getting ready), not make(). The worst tick during the first request was 13.2 late on 1 worker
// and 5.8 on 4 when the pool made the context on a worker as it handshook, 0.7 and 0.4 once it made it (and warmed it
// up) with run_blocking() (0.3 idle; the request took about 16 either way).
//
// Since the system's store is loaded once per process and shared (medians of 5 processes each, ms, before and after):
// "make-insecure" 4.7 then 0.0, as such a context loads no store; "second": the second context's make() 0.1 then 0.0,
// its warm-up 0.1 either way, its handshake 1.3 then 1.2: on macOS a second keychain store was already cheap
// (Security.framework is ready by then, process-wide). On Linux (Arch in Docker on the same laptop, Arch's Botan 3.13,
// its bundle of 121 CAs), where the store is a Flatfile_Certificate_Store parsing the whole bundle, the second make()
// went from 10.7 to 0.0, an insecure context's from 11.6 to 0.0, and the second context's ("second") from 12.1 to 0.0;
// the first stays about 11.8.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <new>
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

#ifdef __APPLE__
#    include <malloc/malloc.h>
#else
#    include <malloc.h>
#endif

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
#include "http/client.hpp"
#include "http/connection_pool.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "tls/tls.hpp"
#include "tls/version.hpp"

// NOLINTBEGIN(misc-new-delete-overloads,cppcoreguidelines-no-malloc,cppcoreguidelines-avoid-non-const-global-variables,readability-inconsistent-declaration-parameter-name):
// counting allocations, and the bytes they hold, by replacing these.
namespace
{

// the heap allocations so far, on any thread; the bytes held now, and the most held since peak was last reset.
std::atomic<std::size_t> allocations = 0;
std::atomic<std::size_t> held        = 0;
std::atomic<std::size_t> peak        = 0;

// NOLINTNEXTLINE(misc-const-correctness): glibc's malloc_usable_size() takes a void*.
std::size_t size_of(void* p)
{
#ifdef __APPLE__
    return malloc_size(p);
#else
    return malloc_usable_size(p);
#endif
}

}

void* operator new(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order::relaxed);
    void* p = std::malloc(size);
    if (p == nullptr) throw std::bad_alloc{};

    const auto now  = held.fetch_add(size_of(p), std::memory_order::relaxed) + size_of(p);
    auto       most = peak.load(std::memory_order::relaxed);
    while (now > most && !peak.compare_exchange_weak(most, now, std::memory_order::relaxed))
    {}
    return p;
}

void operator delete(void* p) noexcept
{
    if (p == nullptr) return;
    held.fetch_sub(size_of(p), std::memory_order::relaxed);
    std::free(p);
}

void operator delete(void* p, std::size_t /*size*/) noexcept { operator delete(p); }
// NOLINTEND(misc-new-delete-overloads,cppcoreguidelines-no-malloc,cppcoreguidelines-avoid-non-const-global-variables,readability-inconsistent-declaration-parameter-name)

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

// writes is what a run of large writes cost: time and allocations per write, and the most the heap held meanwhile
// (over what it held before; through operator new: Botan's own secure allocations aren't counted).
struct writes
{
    double      us_per_write     = -1;
    double      mib_per_second   = 0;
    double      allocs_per_write = 0;
    std::size_t peak_bytes       = 0;
};

// total is how much each run of writes sends, whatever their size.
constexpr std::size_t total_written = std::size_t{256} << 20;

// write_all_of has the server's stream write count writes of size bytes each, once the client raises go (to 2).
task<> write_all_of(net::io::scheduler*         sched,
                    net::listener*              listener,
                    net::tls::server_context    ctx,
                    std::size_t                 size,
                    std::size_t                 count,
                    std::atomic<std::uint64_t>* go)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value()) co_return;
    nodelay(*accepted);

    auto s = co_await net::tls::stream::accept(std::move(*accepted), ctx, net::io::clock::now() + 5s);
    if (!s.has_value()) co_return;

    // (made before the client measures: go is 1 once it is, and the client raises it to 2.)
    const std::vector<std::byte> data(size, std::byte{'x'});
    go->store(1);
    sched->notify_all(*go);
    while (go->load() < 2) (void)co_await sched->wait_on(*go, 1);
    for (std::size_t i = 0; i < count; ++i)
    {
        if ((co_await s->write(data)).err)
        {
            std::println(stderr, "a write failed");
            break;
        }
    }
    co_await s->close();
}

// read_writes connects, raises go to 2 once the server's ready (1), and reads what the server writes, measuring it.
task<writes> read_writes(net::io::scheduler*         sched,
                         std::uint16_t               port,
                         net::tls::client_context    ctx,
                         std::size_t                 size,
                         std::size_t                 count,
                         std::atomic<std::uint64_t>* go)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    if (!conn.has_value()) co_return writes{};
    nodelay(*conn);

    auto s = co_await net::tls::stream::connect(std::move(*conn), ctx, "localhost", net::io::clock::now() + 5s);
    if (!s.has_value()) co_return writes{};

    std::vector<std::byte> buffer(std::size_t{64} * 1'024);
    while (go->load() < 1) (void)co_await sched->wait_on(*go, 0);
    const auto start_held = held.load();
    peak.store(start_held);
    const auto at    = allocations.load();
    const auto start = std::chrono::steady_clock::now();

    go->store(2);
    sched->notify_all(*go);

    std::size_t got = 0;
    while (got < size * count)
    {
        const auto res = co_await s->read(buffer);
        got += res.count;
        if (res.err) break;
    }

    const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    if (got != size * count) co_return writes{};
    co_return writes{
        .us_per_write     = elapsed / static_cast<double>(count),
        .mib_per_second   = static_cast<double>(got) / elapsed * 1e6 / (1'024.0 * 1'024.0),
        .allocs_per_write = static_cast<double>(allocations.load() - at) / static_cast<double>(count),
        .peak_bytes       = peak.load() - start_held,
    };
}

// measure_writes is a run of writes of size bytes (best of 3, by time; the largest peak of the 3).
writes measure_writes(const net::tls::server_options& server, const net::tls::client_options& client, std::size_t size)
{
    auto sctx = net::tls::server_context::make(server);
    auto cctx = net::tls::client_context::make(client);
    if (!sctx.has_value() || !cctx.has_value()) return {};

    writes out;
    for (int round = 0; round < 3; ++round)
    {
        net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
        auto               listener = net::listener::make(&sched, "127.0.0.1", "0");
        if (!listener.has_value()) return {};

        const auto                 count = total_written / size;
        std::atomic<std::uint64_t> go    = 0;
        if (!sched.spawn(write_all_of(&sched, &*listener, *sctx, size, count, &go))) return {};

        const auto w = sched.run_to_completion(read_writes(&sched, listener->local_port(), *cctx, size, count, &go));
        if (w.us_per_write < 0) return {};
        const auto most = std::max(out.peak_bytes, w.peak_bytes);
        if (out.us_per_write < 0 || w.us_per_write < out.us_per_write) out = w;
        out.peak_bytes = most;
    }
    return out;
}

// ms_since is how long it's been since start, in ms.
double ms_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// accept_handshakes accepts connections until accepting fails (the scheduler stops), running each one's handshake,
// then closing it; whether it succeeds is the client's business.
task<> accept_handshakes(net::listener* listener, net::tls::server_context ctx)
{
    while (true)
    {
        auto accepted = co_await listener->accept();
        if (!accepted.has_value()) co_return;
        nodelay(*accepted);

        auto s = co_await net::tls::stream::accept(std::move(*accepted), ctx, net::io::clock::now() + 5s);
        if (s.has_value()) co_await s->close();
    }
}

// handshake_once connects and runs the client's handshake, returning how long that took (ms), and how it failed.
task<std::pair<double, std::error_code>>
handshake_once(net::io::scheduler* sched, std::uint16_t port, net::tls::client_context ctx)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    if (!conn.has_value()) co_return std::pair{-1.0, conn.error()};
    nodelay(*conn);

    const auto start = std::chrono::steady_clock::now();
    auto       s = co_await net::tls::stream::connect(std::move(*conn), ctx, "localhost", net::io::clock::now() + 30s);
    const auto took = ms_since(start);
    if (!s.has_value()) co_return std::pair{took, s.error()};

    co_await s->close();
    co_return std::pair{took, std::error_code{}};
}

int context_make(bool insecure)
{
    for (int i = 1; i <= 2; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto ctx   = net::tls::client_context::make({.insecure_skip_verification = insecure});
        const auto took  = ms_since(start);
        if (!ctx.has_value())
        {
            std::println(stderr, "make: {}", ctx.error().message());
            return 1;
        }
        std::println("make {}: {:.1f} ms", i, took);
    }
    return 0;
}

int context_verify(const pki& server, bool warm)
{
    auto sctx     = net::tls::server_context::make({.certificate_chain = server.chain, .private_key = server.key});
    auto trusting = net::tls::client_context::make({.trusted_ca_file = server.ca});

    auto start  = std::chrono::steady_clock::now();
    auto system = net::tls::client_context::make({});
    std::println("make, system store: {:.1f} ms", ms_since(start));
    if (!sctx.has_value() || !trusting.has_value() || !system.has_value())
    {
        std::println(stderr, "a context couldn't be made");
        return 1;
    }

    // (with "verify-warm", its store's first lookup before any handshake, as http::client's pool has it.)
    if (warm)
    {
        start = std::chrono::steady_clock::now();
        net::tls::detail::warm_up(*system);
        std::println("warm-up: {:.1f} ms", ms_since(start));
    }

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0");
    if (!listener.has_value() || !sched.spawn(accept_handshakes(&*listener, *sctx))) return 1;

    const auto run = [](net::io::scheduler*      sched,
                        std::uint16_t            port,
                        net::tls::client_context trusting,
                        net::tls::client_context system) -> task<std::vector<std::pair<double, std::error_code>>>
    {
        std::vector<std::pair<double, std::error_code>> out;
        out.push_back(co_await handshake_once(sched, port, trusting));
        out.push_back(co_await handshake_once(sched, port, system));
        out.push_back(co_await handshake_once(sched, port, system));
        co_return out;
    };

    const auto took = sched.run_to_completion(run(&sched, listener->local_port(), *trusting, *system));
    const auto show = [](const std::pair<double, std::error_code>& h)
    { return std::format("{:.1f} ms ({})", h.first, h.second ? h.second.message() : "ok"); };

    std::println("handshake, trusted CA file: {}", show(took[0]));
    std::println("handshake, system store, first: {}", show(took[1]));
    std::println("handshake, system store, second: {}", show(took[2]));
    return 0;
}

int context_second(const pki& server)
{
    auto sctx  = net::tls::server_context::make({.certificate_chain = server.chain, .private_key = server.key});
    auto first = net::tls::client_context::make({});
    if (!sctx.has_value() || !first.has_value())
    {
        std::println(stderr, "a context couldn't be made");
        return 1;
    }
    net::tls::detail::warm_up(*first);

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0");
    if (!listener.has_value() || !sched.spawn(accept_handshakes(&*listener, *sctx))) return 1;

    // (the second context's made in the task, as the scheduler stops once it's done; it blocks a worker meanwhile.)
    const auto run = [](net::io::scheduler*      sched,
                        std::uint16_t            port,
                        net::tls::client_context first) -> task<std::vector<std::string>>
    {
        const auto show = [](const std::pair<double, std::error_code>& h)
        { return std::format("{:.1f} ms ({})", h.first, h.second ? h.second.message() : "ok"); };

        std::vector<std::string> out;
        const auto               by_first = co_await handshake_once(sched, port, first);
        out.push_back("handshake, first context: " + show(by_first));

        auto start  = std::chrono::steady_clock::now();
        auto second = net::tls::client_context::make({});
        out.push_back(std::format("make, second context: {:.1f} ms", ms_since(start)));
        if (!second.has_value()) co_return out;

        start = std::chrono::steady_clock::now();
        net::tls::detail::warm_up(*second);
        out.push_back(std::format("warm-up, second context: {:.1f} ms", ms_since(start)));

        const auto by_second = co_await handshake_once(sched, port, *second);
        out.push_back("handshake, second context: " + show(by_second));
        co_return out;
    };

    for (const auto& line : sched.run_to_completion(run(&sched, listener->local_port(), *first)))
        std::println("{}", line);
    return 0;
}

// ticks is what a ticker saw: when each tick was due, and how late it came; only the ticker touches them until it's
// done. ticked is the latest's due time (since the clock's epoch), for a task to wait on.
struct ticks
{
    std::vector<std::pair<net::io::clock::time_point, net::io::clock::duration>> seen;
    std::atomic<std::uint64_t>                                                   ticked   = 0;
    std::atomic<bool>                                                            stopping = false;
    std::atomic<std::uint64_t>                                                   done     = 0;

    // worst_ms is the latest a tick due within [from, to] came, in ms.
    [[nodiscard]] double worst_ms(net::io::clock::time_point from, net::io::clock::time_point to) const
    {
        net::io::clock::duration worst{};
        for (const auto& [due, late] : seen)
        {
            if (due >= from && due <= to) worst = std::max(worst, late);
        }
        return std::chrono::duration<double, std::milli>(worst).count();
    }
};

task<> tick(net::io::scheduler* sched, ticks* t)
{
    t->seen.reserve(100'000);
    while (!t->stopping.load())
    {
        const auto due = net::io::clock::now() + 1ms;
        if (co_await sched->sleep_until(due)) break;

        t->seen.emplace_back(due, net::io::clock::now() - due);
        t->ticked.store(static_cast<std::uint64_t>(due.time_since_epoch().count()));
        sched->notify_all(t->ticked);
    }

    t->done.store(1);
    sched->notify_all(t->done);
}

int context_ticker(const pki& server, std::size_t workers)
{
    auto sctx = net::tls::server_context::make({.certificate_chain = server.chain, .private_key = server.key});
    if (!sctx.has_value()) return 1;

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(workers)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0");
    if (!listener.has_value() || !sched.spawn(accept_handshakes(&*listener, *sctx))) return 1;

    // the system's trust store, as by default.
    net::http::client client{&sched};

    struct result
    {
        double          idle_worst_ms = 0;
        double          worst_ms      = 0;
        double          request_ms    = 0;
        std::error_code err;
    };

    const auto run = [](net::io::scheduler* sched, net::http::client* client, std::string url) -> task<result>
    {
        ticks t;
        if (!sched->spawn(tick(sched, &t))) co_return result{.err = std::make_error_code(std::errc::interrupted)};

        // as long with nothing else to do, for what the ticks are late by anyway.
        const auto idle = net::io::clock::now();
        (void)co_await sched->sleep_until(idle + 300ms);

        const auto start = net::io::clock::now();
        auto       resp  = co_await client->get(url);
        const auto end   = net::io::clock::now();

        // (until a tick due after the request has come: one held up by it comes after it's done.)
        while (true)
        {
            const auto seen = t.ticked.load();
            if (std::cmp_greater(seen, end.time_since_epoch().count())) break;
            (void)co_await sched->wait_on(t.ticked, seen);
        }
        t.stopping.store(true);
        while (t.done.load() == 0) (void)co_await sched->wait_on(t.done, 0);

        co_return result{
            .idle_worst_ms = t.worst_ms(idle, start),
            .worst_ms      = t.worst_ms(start, end),
            .request_ms    = std::chrono::duration<double, std::milli>(end - start).count(),
            .err           = resp.has_value() ? std::error_code{} : resp.error(),
        };
    };

    const auto r = sched.run_to_completion(
        run(&sched, &client, "https://localhost:" + std::to_string(listener->local_port()) + "/"));
    std::println("workers {}: worst tick lateness {:.1f} ms during the first request ({:.1f} ms, {}); idle {:.1f} ms",
                 workers,
                 r.worst_ms,
                 r.request_ms,
                 r.err ? r.err.message() : "ok",
                 r.idle_worst_ms);
    return 0;
}

}

int main(int argc, char** argv)
{
    // (only the large writes, with "writes".)
    const auto args         = std::span{argv, static_cast<std::size_t>(argc)};
    const bool writes_alone = args.size() > 1 && std::string_view{args[1]} == "writes";

    const auto dir =
        std::filesystem::temp_directory_path() / ("cppnet-bench-tls-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);

    // (the TLS context's first load: see the top of the file.)
    if (args.size() > 2 && std::string_view{args[1]} == "context")
    {
        const std::string_view part = args[2];
        int                    code = 2;
        if (part == "make") code = context_make(false);
        else if (part == "make-insecure") code = context_make(true);
        else if (part == "verify") code = context_verify(make_pki(dir, "ecdsa", false), false);
        else if (part == "verify-warm") code = context_verify(make_pki(dir, "ecdsa", false), true);
        else if (part == "second") code = context_second(make_pki(dir, "ecdsa", false));
        else if (part == "ticker" && args.size() > 3)
            code = context_ticker(make_pki(dir, "ecdsa", false), std::stoul(std::string{args[3]}));
        else
            std::println(stderr,
                         "usage: bench_tls context make|make-insecure|verify|verify-warm|second|ticker <workers>");

        std::filesystem::remove_all(dir);
        return code;
    }

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

    if (!writes_alone)
    {
        std::println("{:<24} {:>18} {:>18} {:>18}", "server key", "full, no tickets", "full", "resumed");
        for (const auto& s : setups)
        {
            const auto without = measure(s.server, s.client, mode::full_without_tickets);
            const auto full    = measure(s.server, s.client, mode::full);
            const auto resumed = measure(s.server, s.client, mode::resumed);
            std::println("{:<24} {:>18} {:>18} {:>18}", s.name, show(without), show(full), show(resumed));
        }

        std::println("{:<24} {:>18}", "TCP alone", show(measure_tcp()));
    }

    // large writes: the server writes 256 MiB in writes of each size, the client reads it (ECDSA, TLS 1.3).
    std::println("\n{:<14} {:>14} {:>12} {:>18} {:>14}",
                 "write size",
                 "us per write",
                 "MiB/s",
                 "allocs per write",
                 "peak KiB");
    for (const std::size_t size : {std::size_t{16} << 10,
                                   std::size_t{64} << 10,
                                   std::size_t{256} << 10,
                                   std::size_t{1} << 20,
                                   std::size_t{16} << 20})
    {
        const auto w = measure_writes(setups[0].server, setups[0].client, size);
        std::println("{:<14} {:>14.1f} {:>12.0f} {:>18.2f} {:>14}",
                     std::format("{} KiB", size >> 10),
                     w.us_per_write,
                     w.mib_per_second,
                     w.allocs_per_write,
                     w.peak_bytes >> 10);
    }

    std::filesystem::remove_all(dir);
}
