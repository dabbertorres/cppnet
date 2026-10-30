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

// measured is a run's: how long it took, and the allocations it made.
struct measured
{
    std::chrono::steady_clock::duration took{};
    std::size_t                         allocations = 0;
    bool                                failed      = false;
};

// echo accepts a WebSocket, and echoes its messages whole until it ends.
task<> echo(const http::server_request& req, http::response_writer& rw)
{
    auto conn = co_await ws::accept(req, rw, {.connection = {.max_message = std::uint64_t{1} << 20}});
    if (!conn.has_value()) co_return;

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

    http::client client{&scheduler};
    const auto   url = std::format("ws://127.0.0.1:{}/ws", server.local_port());

    http::client_options h2;
    h2.http2_prior_knowledge = true;
    http::client h2_client{&scheduler, h2};

    // (one scheduler, so both runs are one task.)
    const auto both = [](net::io::scheduler* sched,
                         http::client*       h1,
                         http::client*       h2,
                         std::string         url,
                         std::string         warm) -> task<std::vector<measured>>
    {
        auto best = co_await run(sched, h1, url);
        auto over = co_await run(sched, h2, url, warm);
        best.insert(best.end(), over.begin(), over.end());
        co_return best;
    };
    const auto best = scheduler.run_to_completion(
        both(&scheduler, &client, &h2_client, url, std::format("http://127.0.0.1:{}/", server.local_port())));
    if (best.size() != loads.size() * 4) return 1;

    std::println("cppnet's client and server, over loopback ({} workers; best of {}):",
                 workers->concurrency(),
                 repeats);
    for (std::size_t i = 0; i < best.size(); ++i)
    {
        const auto&       l       = loads[(i / 2) % loads.size()];
        const auto        per     = std::chrono::duration<double, std::micro>(best[i].took).count() / l.count;
        const auto        allocs  = static_cast<double>(best[i].allocations) / l.count;
        const auto* const how     = i % 2 == 0 ? "one at a time" : "in flight";
        const auto* const version = i < loads.size() * 2 ? "HTTP/1.1" : "HTTP/2";
        const auto        mib     = static_cast<double>(l.size) / per * 1e6 / (1'024.0 * 1'024.0);
        std::println("{}, {}, {}:", version, l.name, how);
        std::println(
            "{:>10.2f} us per message ({:.0f} messages/s, {:.1f} MiB/s each way), {:.2f} allocations per message",
            per,
            1e6 / per,
            mib,
            allocs);
    }

    server.close();
    scheduler.shutdown();
    return 0;
}
