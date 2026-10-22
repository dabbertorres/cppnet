// TCP_NOTSENT_LOWAT on the server's HTTP/2 connections (docs/plans/http2.md, stage 6's fourth part): set on each
// HTTP/2 connection's socket (not on HTTP/1.1's), and the writer waiting for the socket to take more before each round,
// so that what it frames is in the scheduler's order up to the last moment.
//
// What the kernel holds depends on its tuning, so the writer's waits are tested through the server's hook
// (server_config::http2_writable), which holds the connection "unwritable" as the test says.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/socket.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/h2_client.hpp"
#include "http/hpack.hpp"
#include "http/http.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
#include "http/http2_server.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;
namespace hpack = net::http::hpack;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::h2_client;
using test::stop_probe;
using test::writer_gate;

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

constexpr std::size_t frame = 16'384; // the client's MAX_FRAME_SIZE: the server's DATA frames' size

// handlers is what the handlers share with the test: how many /queued probes have seen the streams queue.
struct handlers
{
    net::io::scheduler*        sched = nullptr;
    std::atomic<std::uint64_t> queued{0};
};

// make_router answers /bytes with x-size bytes (with a Content-Length), written at once; /queued (the probe) once
// x-count streams wait in the connection's scheduler, counting it (in handlers::queued) before it answers, as its
// answer may be held with the writer; and /mark with the TCP_NOTSENT_LOWAT of the connection's socket.
http::router make_router(handlers* h)
{
    http::router r;
    r.GET("/bytes",
          [](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              const auto size = std::stoul(std::string{req.headers.get("x-size"sv).value_or("0"sv)});
              auto*      body = co_await resp.send(http::status::OK, size);

              const std::string data(size, 'x');
              (void)co_await body->write(std::as_bytes(std::span{data}));
          });
    r.GET("/queued",
          [h](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              const auto n      = std::stoul(std::string{req.headers.get("x-count"sv).value_or("0"sv)});
              const bool queued = co_await http::detail::wait_queued_streams(req, n);
              if (queued) stop_probe::bump(h->sched, h->queued);
              resp.headers().set("x-queued", queued ? "yes" : "no");
              co_await resp.send(http::status::OK, 0);
          });
    r.GET("/mark",
          [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          {
              auto* body = co_await resp.send(http::status::OK);

              int        value = -1;
              socklen_t  size  = sizeof(value);
              const bool got  = ::getsockopt(body->native_handle(), IPPROTO_TCP, TCP_NOTSENT_LOWAT, &value, &size) == 0;
              const auto text = got ? std::to_string(value) : "(getsockopt failed)";
              (void)co_await body->write(std::as_bytes(std::span{text}));
          });
    return r;
}

struct test_server
{
    test_server(net::io::scheduler* sched, handlers* h, const http::server_config& cfg = {})
        : server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), make_router(h), cfg}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    [[nodiscard]] std::string port() const { return std::to_string(server.local_port()); }

    http::server server;
};

// open_windows is the client's session: windows that don't run out (a stream's 16 MiB, the connection's 16 MiB), so
// only the server's writer holds its DATA back.
http2::session_config open_windows()
{
    http2::session_config config;
    config.local.initial_window_size = std::uint32_t{16} << 20;
    config.connection_window         = std::uint32_t{16} << 20;
    return config;
}

// request_bytes requests size bytes from /bytes, with a Priority field of priority, and sends it.
task<http2::stream_id> request_bytes(h2_client& c, std::size_t size, std::string_view priority)
{
    const auto                      length = std::to_string(size);
    const std::vector<hpack::field> fields{
        {  .name = "x-size",   .value = length},
        {.name = "priority", .value = priority}
    };
    const auto id = c.request("GET", "/bytes", {}, true, fields);
    (void)co_await c.pump([] { return true; }); // (what's to be written is, first)
    co_return id;
}

// probe asks /queued to count once n streams wait in the server's scheduler, and waits (up to 5 seconds) until the n-th
// probe has, returning whether it did.
task<bool> probe(h2_client& c, handlers* h, std::size_t n, std::uint64_t nth)
{
    const auto                      count = std::to_string(n);
    const std::vector<hpack::field> fields{
        {.name = "x-count", .value = count}
    };
    (void)c.request("GET", "/queued", {}, true, fields);
    (void)co_await c.pump([] { return true; });
    co_return co_await stop_probe::reach(h->sched, h->queued, nth);
}

// frames is the DATA frames received, as their streams' letters (names' letters, in ids' order; '?' for others').
std::string frames(const h2_client& c, std::span<const http2::stream_id> ids, std::string_view names)
{
    std::string out;
    for (const auto& [id, _] : c.data_log)
    {
        const auto it = std::ranges::find(ids, id);
        out += it == ids.end() ? '?' : names.at(static_cast<std::size_t>(it - ids.begin()));
    }
    return out;
}

// fetch_mark GETs /mark, returning its body (the server's mark on the connection), or the error.
task<std::string> fetch_mark(http::client* client, std::string url)
{
    auto resp = co_await client->get(url);
    if (!resp.has_value()) co_return "error: " + resp.error().message();

    auto body = co_await net::io::read_all(*resp->body);
    if (!body.has_value()) co_return "error: " + body.error().message();
    co_return std::to_string(resp->version.major) + ": " + *body;
}

}

TEST_CASE("the server sets TCP_NOTSENT_LOWAT on HTTP/2 connections, not on HTTP/1.1's",
          "[http][http2][server][notsent-lowat]")
{
    struct test_case
    {
        std::string_view name;
        bool             http2 = true;
        std::size_t      mark  = std::size_t{16} * 1'024; // the server's http2_notsent_lowat
        std::string_view want;
    };

    const auto tc = GENERATE(test_case{.name = "HTTP/2, by default", .want = "2: 16384"},
                             test_case{.name = "HTTP/2, a mark of its own", .mark = 4'096, .want = "2: 4096"},
                             test_case{.name = "HTTP/2, none", .mark = 0, .want = "2: 0"},
                             test_case{.name = "HTTP/1.1", .http2 = false, .want = "1: 0"});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    handlers           h{.sched = &sched};
    test_server        server{&sched, &h, {.http2_notsent_lowat = tc.mark}};

    // HTTP/2 in cleartext with prior knowledge (its preface), HTTP/1.1 otherwise.
    http::client_options options;
    options.http2                 = tc.http2;
    options.http2_prior_knowledge = tc.http2;
    http::client client{&sched, options};

    const auto out = sched.run_to_completion(fetch_mark(&client, "http://127.0.0.1:" + server.port() + "/mark"));
    CHECK(out == tc.want);
}

TEST_CASE("while the socket can't take more, HTTP/2 responses queue, then go most urgent first",
          "[http][http2][server][notsent-lowat][scheduler]")
{
    writer_gate        gate;
    net::io::scheduler sched{make_pool()};
    gate.sched = &sched;
    handlers    h{.sched = &sched};
    test_server server{&sched, &h, {.http2_writable = gate.hook()}};

    sched.run_to_completion(
        [](net::io::scheduler* sched, std::string port, writer_gate* gate, handlers* h) -> task<>
        {
            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
            REQUIRE(conn.has_value());
            h2_client c{&*conn, &*conn, &*conn, http2::role::client, open_windows()};

            // the settings exchanged, and the writer's last round done.
            bool ok = co_await c.sync();
            REQUIRE(ok);
            c.data_log.clear();

            // from now on, the socket "can't take more" once the writer's written. A bulk download first, which
            // goes at once as far as max_ahead (64 KiB) and queues the rest, as nothing else is queued; the writer
            // writes its head (and that, if it's framed by then), then waits.
            gate->hold();
            std::vector<http2::stream_id> ids;
            ids.push_back(co_await request_bytes(c, 16 * frame, "u=7"));
            ok = co_await probe(c, h, 1, 1);
            REQUIRE(ok);
            ok = co_await stop_probe::reach(sched, gate->held, 1);
            REQUIRE(ok);

            // then a more urgent one, and a usual one, which queue behind it: nothing's framed while the writer waits.
            ids.push_back(co_await request_bytes(c, 2 * frame, "u=0"));
            ids.push_back(co_await request_bytes(c, 2 * frame, "u=3"));
            ok = co_await probe(c, h, 3, 2);
            REQUIRE(ok);

            // let go: the bulk download's first 64 KiB were framed, the rest's the scheduler's.
            gate->open();
            ok = co_await c.pump([&]
                                 { return std::ranges::all_of(ids, [&](auto id) { return c.responses[id].done; }); });
            REQUIRE(ok);
            CHECK(frames(c, ids, "BUN") == "BBBBUUNNBBBBBBBBBBBB");
            for (const auto id : ids) CHECK_FALSE(c.responses[id].reset);
            CHECK(c.responses[ids[0]].body.size() == 16 * frame);
        }(&sched, server.port(), &gate, &h));
}

TEST_CASE("an HTTP/2 response waiting its turn doesn't time out while the writer waits for the socket",
          "[http][http2][server][notsent-lowat][scheduler][timeouts]")
{
    writer_gate        gate;
    net::io::scheduler sched{make_pool()};
    gate.sched = &sched;
    handlers    h{.sched = &sched};
    test_server server{
        &sched,
        &h,
        {.write_timeout = 1s, .http2_writable = gate.hook()}
    };

    sched.run_to_completion(
        [](net::io::scheduler* sched, std::string port, writer_gate* gate, handlers* h) -> task<>
        {
            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
            REQUIRE(conn.has_value());
            h2_client c{&*conn, &*conn, &*conn, http2::role::client, open_windows()};

            bool ok = co_await c.sync();
            REQUIRE(ok);
            c.data_log.clear();

            // an urgent response (which goes at once as far as max_ahead, and queues the rest), then a bulk one
            // waiting its turn behind it.
            gate->hold();
            constexpr std::size_t         big = std::size_t{4} << 20;
            std::vector<http2::stream_id> ids;
            ids.push_back(co_await request_bytes(c, big, "u=0"));
            ok = co_await probe(c, h, 1, 1);
            REQUIRE(ok);
            ids.push_back(co_await request_bytes(c, frame, "u=7"));
            ok = co_await probe(c, h, 2, 2);
            REQUIRE(ok);

            // the socket takes a round (64 KiB) every 20 ms, so the urgent response takes the connection for about
            // 1.3 seconds; and once, for 600 ms (under the write timeout), it takes nothing. So the bulk one waits for
            // its turn about 2 seconds, past its write timeout of 1, while the writer waits for the socket, and
            // writes when it can.
            std::atomic<std::uint64_t> pacing_done{0};
            std::atomic_bool           stop_pacing{false};
            const auto                 pace = [](net::io::scheduler*         sched,
                                                 writer_gate*                gate,
                                                 const std::atomic_bool*     stop,
                                                 std::atomic<std::uint64_t>* done) -> task<>
            {
                for (int round = 0; !stop->load(); ++round)
                {
                    const auto pause = round == 30 ? 600ms : 20ms;
                    (void)co_await sched->sleep_until(net::io::clock::now() + pause);
                    gate->grant();
                }
                stop_probe::bump(sched, *done);
            };
            const bool spawned = sched->spawn(pace(sched, gate, &stop_pacing, &pacing_done));
            REQUIRE(spawned);

            ok          = co_await c.pump([&] { return c.responses[ids[1]].done; });
            stop_pacing = true;
            gate->open();
            const bool paced = co_await stop_probe::reach(sched, pacing_done, 1);
            REQUIRE(ok);
            REQUIRE(paced);

            const auto& bulk = c.responses[ids[1]];
            CAPTURE(bulk.reset.message());
            CHECK_FALSE(bulk.reset);
            CHECK(bulk.body.size() == frame);
            CHECK(c.responses[ids[0]].body.size() == big);
            CHECK(frames(c, ids, "UB").find('B') == big / frame);
        }(&sched, server.port(), &gate, &h));
}
