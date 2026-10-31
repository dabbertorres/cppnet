// bench_websocket measures WebSocket messages over loopback, cppnet's client against cppnet's server (a handler that
// echoes each message whole): the time per message, and the heap allocations per message (the client's and the
// server's together, as they share the process), for small messages (16 bytes of text) and large ones (64 KiB, binary:
// past both sides' 16 KiB buffers, so the client masks them a buffer's worth at a time, and the server reads them
// straight into the message). Two ways:
// - one at a time: the client writes a message, and reads its echo, before the next (a round trip each), as a client
//   waiting for each answer does;
// - in flight: one task writes the messages while another reads their echoes, so as many are on the way as the
//   connection holds (until the writes wait for the server to read).
// Each over HTTP/1.1 (the WebSocket's own connection), then over HTTP/2 (RFC 8441: a stream of the client's connection,
// over cleartext with prior knowledge, made by a request first).
//
// Then what HTTP/2 is for: N WebSockets at once (8, then 64), each with a task of its own (one at a time, or in flight:
// a writer and a reader each), over N HTTP/1.1 connections, then as N streams of one HTTP/2 connection. Their time per
// message is the run's (from the first task's start to the last's end) over all the WebSockets' messages; and the
// memory each holds is the heap's growth (both sides', as the allocator counts it: all of the process's threads), from
// before the client connects (each of these runs has a client of its own, so HTTP/2's connection is counted too) to
// once they're open (and have each made a round trip, so both sides' reads wait, as an idle WebSocket's do), and to
// after the runs (still open: what they kept of the runs, as the echo's message, 64 KiB each), divided by N.
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <new>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "config.hpp"
#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "websocket/websocket.hpp"

#ifdef NET_IS_OSX
#    include <malloc/malloc.h>
#elifdef NET_IS_LINUX
#    include <malloc.h>
#endif

// NOLINTBEGIN(misc-new-delete-overloads,cppcoreguidelines-no-malloc,cppcoreguidelines-avoid-non-const-global-variables,readability-inconsistent-declaration-parameter-name):
// counting allocations, by replacing these.
namespace
{

// the heap allocations so far, on any thread.
std::atomic<std::size_t> allocations = 0;
}

void* operator new(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order::relaxed);
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t /*size*/) noexcept { std::free(p); }
// NOLINTEND(misc-new-delete-overloads,cppcoreguidelines-no-malloc,cppcoreguidelines-avoid-non-const-global-variables,readability-inconsistent-declaration-parameter-name)

namespace
{

namespace http = net::http;
namespace ws   = net::websocket;

using net::coro::task;

constexpr int repeats = 5;
constexpr int warm_up = 100;

// what's measured: the messages' size and type, and how many each run sends.
struct load
{
    std::string_view name;
    std::size_t      size;
    ws::message_type type;
    int              count;
};

constexpr std::array loads{
    load{     .name = "small (16 B text)",.size = 16,.type = ws::message_type::text,.count = 20'000       },
    load{.name  = "large (64 KiB binary)",
         .size  = std::size_t{64} * 1'024,
         .type  = ws::message_type::binary,
         .count = 2'000},
};

// how many WebSockets the parallel runs open at once.
constexpr std::array widths{std::size_t{8}, std::size_t{64}};

// measured is a run's: how long it took, and the allocations it made.
struct measured
{
    std::chrono::steady_clock::duration took{};
    std::size_t                         allocations = 0;
    bool                                failed      = false;
};

// the WebSockets the server has accepted over HTTP/2, so a parallel run can tell that all of its went over its one
// connection (rather than some over HTTP/1.1, as a client does when its connection has no stream free).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<std::size_t> accepted_over_http2 = 0;

// heap_in_use is how much of the heap the process's allocations hold (all of its threads'), as the allocator counts it
// (on macOS and Linux; 0 elsewhere).
std::size_t heap_in_use()
{
#ifdef NET_IS_OSX
    malloc_statistics_t stats{};
    malloc_zone_statistics(nullptr, &stats);
    return stats.size_in_use;
#elifdef NET_IS_LINUX
    const auto info = mallinfo2();
    return info.uordblks + info.hblkhd;
#else
    return 0;
#endif
}

// echo accepts a WebSocket, and echoes its messages whole until it ends.
task<> echo(const http::server_request& req, http::response_writer& rw)
{
    auto conn = co_await ws::accept(req, rw, {.connection = {.max_message = std::uint64_t{1} << 20}});
    if (!conn.has_value()) co_return;
    if (req.version.major == 2) accepted_over_http2.fetch_add(1);

    std::string message;
    while (true)
    {
        const auto type = co_await conn->read_message(message);
        if (!type.has_value()) co_return;
        if (co_await conn->write_message(*type, message)) co_return;
    }
}

// round_trips writes count messages, each once the last's echo has been read.
task<measured> round_trips(ws::connection* conn, load l, int count)
{
    const std::string out(l.size, 'x');
    std::string       in;
    in.reserve(l.size);

    measured   result;
    const auto at    = allocations.load();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < count; ++i)
    {
        if (co_await conn->write_message(l.type, out)) result.failed = true;
        if (!(co_await conn->read_message(in)).has_value() || in.size() != l.size) result.failed = true;
        if (result.failed) break;
    }
    result.took        = std::chrono::steady_clock::now() - start;
    result.allocations = allocations.load() - at;
    co_return result;
}

// write_all writes count messages, as fast as the connection takes them, then raises done.
task<> write_all(net::io::scheduler* sched, ws::connection* conn, load l, int count, std::atomic<std::uint64_t>* done)
{
    const std::string out(l.size, 'x');
    for (int i = 0; i < count; ++i)
    {
        if (co_await conn->write_message(l.type, out)) break;
    }
    done->store(1);
    sched->notify_all(*done);
}

// in_flight writes count messages from a task of their own, while reading their echoes.
task<measured> in_flight(net::io::scheduler* sched, ws::connection* conn, load l, int count)
{
    std::string in;
    in.reserve(l.size);

    std::atomic<std::uint64_t> done = 0;

    measured   result;
    const auto at    = allocations.load();
    const auto start = std::chrono::steady_clock::now();
    if (!sched->spawn(write_all(sched, conn, l, count, &done))) co_return measured{.failed = true};

    for (int i = 0; i < count; ++i)
    {
        if (!(co_await conn->read_message(in)).has_value() || in.size() != l.size)
        {
            result.failed = true;
            break;
        }
    }
    result.took        = std::chrono::steady_clock::now() - start;
    result.allocations = allocations.load() - at;

    while (done.load() == 0) (void)co_await sched->wait_on(done, 0);
    co_return result;
}

// run opens a WebSocket (over HTTP/2 if warm, the URL of a request to send first, so the client has an HTTP/2
// connection for it), and measures each load both ways, repeats times, returning the best (by time) of each.
task<std::vector<measured>> run(net::io::scheduler* sched, http::client* client, std::string url, std::string warm = {})
{
    std::vector<measured> best;

    if (!warm.empty())
    {
        auto resp = co_await client->get(warm);
        if (!resp.has_value() || resp->version.major != 2)
        {
            std::println(stderr, "failed to make an HTTP/2 connection");
            co_return best;
        }
        (void)co_await net::io::read_all(*resp->body);
    }

    auto conn = co_await ws::connect(*client, url, {.connection = {.max_message = std::uint64_t{1} << 20}});
    if (!conn.has_value())
    {
        std::println(stderr, "failed to connect: {}", conn.error().message());
        co_return best;
    }

    for (const auto& l : loads)
    {
        for (const bool flight : {false, true})
        {
            (void)co_await round_trips(&*conn, l, warm_up);

            measured fastest{.took = std::chrono::steady_clock::duration::max()};
            for (int r = 0; r < repeats; ++r)
            {
                const auto m =
                    flight ? co_await in_flight(sched, &*conn, l, l.count) : co_await round_trips(&*conn, l, l.count);
                if (m.failed) std::println(stderr, "{}: a run failed", l.name);
                if (m.took < fastest.took) fastest = m;
            }
            best.push_back(fastest);
        }
    }

    (void)co_await conn->close();
    co_return best;
}

// gang counts the tasks of a parallel run still going, and whether any failed.
struct gang
{
    std::atomic<std::uint64_t> left   = 0;
    std::atomic<bool>          failed = false;
};

// one_of runs one WebSocket's share of a parallel run, then tells g it's done.
task<> one_of(net::io::scheduler* sched, ws::connection* conn, load l, int count, bool flight, gang* g)
{
    const auto m = flight ? co_await in_flight(sched, conn, l, count) : co_await round_trips(conn, l, count);
    if (m.failed) g->failed.store(true);
    if (g->left.fetch_sub(1) == 1) sched->notify_all(g->left);
}

// together runs count messages on each of conns at once (a task each, and a writer of its own in flight), returning
// how long they all took, and the allocations they made.
task<measured> together(net::io::scheduler* sched, std::vector<ws::connection>* conns, load l, int count, bool flight)
{
    gang g;
    g.left = conns->size();

    measured   result;
    const auto at    = allocations.load();
    const auto start = std::chrono::steady_clock::now();
    for (auto& conn : *conns)
    {
        if (!sched->spawn(one_of(sched, &conn, l, count, flight, &g)))
        {
            g.failed.store(true);
            g.left.fetch_sub(1);
        }
    }
    for (auto seen = g.left.load(); seen != 0; seen = g.left.load()) (void)co_await sched->wait_on(g.left, seen);
    result.took        = std::chrono::steady_clock::now() - start;
    result.allocations = allocations.load() - at;
    result.failed      = g.failed.load();
    co_return result;
}

// in_parallel is a parallel run's: the best (by time) of each load both ways, and the heap each WebSocket held once
// open and after the runs, in bytes.
struct in_parallel
{
    std::vector<measured> best;
    double                open  = 0;
    double                after = 0;
};

// per_websocket is how many messages each of n WebSockets sends in a parallel run of count, so that all of them send
// about as many as a run of one does.
int per_websocket(int count, std::size_t n) { return static_cast<int>((static_cast<std::size_t>(count) + n - 1) / n); }

// run_parallel opens n WebSockets (as streams of one HTTP/2 connection if warm, the URL of a request to send first, so
// the client has the connection for them), and measures each load both ways on all of them at once, repeats times.
task<in_parallel>
run_parallel(net::io::scheduler* sched, http::client* client, std::string url, std::size_t n, std::string warm = {})
{
    in_parallel result;
    const auto  before = heap_in_use();
    const auto  over   = accepted_over_http2.load();

    if (!warm.empty())
    {
        auto resp = co_await client->get(warm);
        if (!resp.has_value() || resp->version.major != 2)
        {
            std::println(stderr, "failed to make an HTTP/2 connection");
            co_return result;
        }
        (void)co_await net::io::read_all(*resp->body);
    }

    std::vector<ws::connection> conns;
    conns.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        auto conn = co_await ws::connect(*client, url, {.connection = {.max_message = std::uint64_t{1} << 20}});
        if (!conn.has_value())
        {
            std::println(stderr, "failed to connect: {}", conn.error().message());
            co_return result;
        }
        conns.push_back(std::move(*conn));
    }
    if (const auto h2 = accepted_over_http2.load() - over; h2 != (warm.empty() ? 0 : n))
    {
        std::println(stderr, "{} of {} WebSockets went over HTTP/2", h2, n);
        co_return result;
    }

    if ((co_await together(sched, &conns, loads[0], 1, false)).failed)
        std::println(stderr, "a first round trip failed");
    result.open = (static_cast<double>(heap_in_use()) - static_cast<double>(before)) / static_cast<double>(n);

    for (const auto& l : loads)
    {
        for (const bool flight : {false, true})
        {
            (void)co_await together(sched, &conns, l, per_websocket(warm_up, n), false);

            measured fastest{.took = std::chrono::steady_clock::duration::max()};
            for (int r = 0; r < repeats; ++r)
            {
                const auto m = co_await together(sched, &conns, l, per_websocket(l.count, n), flight);
                if (m.failed) std::println(stderr, "{}: a run failed", l.name);
                if (m.took < fastest.took) fastest = m;
            }
            result.best.push_back(fastest);
        }
    }
    result.after = (static_cast<double>(heap_in_use()) - static_cast<double>(before)) / static_cast<double>(n);

    for (auto& conn : conns) (void)co_await conn.close();
    co_return result;
}

// print_measured prints a run's time and allocations per message, of count messages of l.
void print_measured(const measured& m, load l, std::size_t count)
{
    const auto per    = std::chrono::duration<double, std::micro>(m.took).count() / static_cast<double>(count);
    const auto allocs = static_cast<double>(m.allocations) / static_cast<double>(count);
    const auto mib    = static_cast<double>(l.size) / per * 1e6 / (1'024.0 * 1'024.0);
    std::println("{:>10.2f} us per message ({:.0f} messages/s, {:.1f} MiB/s each way), {:.2f} allocations per message",
                 per,
                 1e6 / per,
                 mib,
                 allocs);
}

}

int main()
{
    spdlog::set_level(spdlog::level::warn);

    auto               workers = std::make_shared<net::coro::thread_pool>(2);
    net::io::scheduler scheduler{workers};

    http::router router;
    router.GET("/ws", echo);
    router.GET("/", [](const http::server_request& /*req*/, http::response_writer& /*rw*/) -> task<> { co_return; });

    http::server server{
        &scheduler,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    scheduler.spawn(server.serve());

    const auto url  = std::format("ws://127.0.0.1:{}/ws", server.local_port());
    const auto warm = std::format("http://127.0.0.1:{}/", server.local_port());

    http::client_options h2;
    h2.http2_prior_knowledge = true;

    // a client for each run, so each parallel run's HTTP/2 connection is its own.
    std::vector<std::unique_ptr<http::client>> clients;
    for (std::size_t i = 0; i < 1 + widths.size(); ++i)
    {
        clients.push_back(std::make_unique<http::client>(&scheduler));
        clients.push_back(std::make_unique<http::client>(&scheduler, h2));
    }

    // (one scheduler, so all of the runs are one task.)
    struct results
    {
        std::vector<measured>    single;
        std::vector<in_parallel> parallel;
    };
    const auto all = [](net::io::scheduler*                         sched,
                        std::vector<std::unique_ptr<http::client>>* clients,
                        std::string                                 url,
                        std::string                                 warm) -> task<results>
    {
        results r;
        r.single  = co_await run(sched, (*clients)[0].get(), url);
        auto over = co_await run(sched, (*clients)[1].get(), url, warm);
        r.single.insert(r.single.end(), over.begin(), over.end());
        for (std::size_t i = 0; i < widths.size(); ++i)
        {
            r.parallel.push_back(co_await run_parallel(sched, (*clients)[2 + (2 * i)].get(), url, widths[i]));
            r.parallel.push_back(co_await run_parallel(sched, (*clients)[3 + (2 * i)].get(), url, widths[i], warm));
        }
        co_return r;
    };
    const auto r = scheduler.run_to_completion(all(&scheduler, &clients, url, warm));
    if (r.single.size() != loads.size() * 4) return 1;
    for (const auto& p : r.parallel)
    {
        if (p.best.size() != loads.size() * 2) return 1;
    }

    std::println("cppnet's client and server, over loopback ({} workers; best of {}):",
                 workers->concurrency(),
                 repeats);
    for (std::size_t i = 0; i < r.single.size(); ++i)
    {
        const auto&       l       = loads[(i / 2) % loads.size()];
        const auto* const how     = i % 2 == 0 ? "one at a time" : "in flight";
        const auto* const version = i < loads.size() * 2 ? "HTTP/1.1" : "HTTP/2";
        std::println("{}, {}, {}:", version, l.name, how);
        print_measured(r.single[i], l, static_cast<std::size_t>(l.count));
    }

    for (std::size_t p = 0; p < r.parallel.size(); ++p)
    {
        const auto        n       = widths[p / 2];
        const auto* const version = p % 2 == 0 ? "HTTP/1.1 (a connection each)" : "HTTP/2 (one connection)";
        std::println("{} WebSockets at once, {}: {:.1f} KiB per WebSocket once open, {:.1f} KiB after the runs",
                     n,
                     version,
                     r.parallel[p].open / 1'024.0,
                     r.parallel[p].after / 1'024.0);
        for (std::size_t i = 0; i < r.parallel[p].best.size(); ++i)
        {
            const auto&       l   = loads[i / 2];
            const auto* const how = i % 2 == 0 ? "one at a time" : "in flight";
            std::println("{} WebSockets at once, {}, {}, {}:", n, version, l.name, how);
            print_measured(r.parallel[p].best[i], l, static_cast<std::size_t>(per_websocket(l.count, n)) * n);
        }
    }

    server.close();
    scheduler.shutdown();
    return 0;
}
