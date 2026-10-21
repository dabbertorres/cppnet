// The client's HTTP/2 request bodies through its connection's scheduler (docs/plans/http2.md, stage 6, part 7): a
// body's write goes at once while nothing contends, and otherwise waits in the scheduler for its turn, every request
// u=3 incremental, so bodies sent at once share the connection a frame each.
//
// To make the order the scheduler's alone, the server the test plays (test::h2_client) holds the connection's window
// shut: a first request's body (the plug) uses up the 65535 bytes a connection starts with, which the server doesn't
// give back; the bodies under test then have no window, so they all queue (which the test waits for, through the
// connection's wait_queued()); then the server gives the window back, a frame's worth at a time where the order's
// checked, so each frame the client sends is the scheduler's choice among every stream that's waiting.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/connection.hpp"
#include "http/h2_client.hpp"
#include "http/http2.hpp"
#include "http/http2_client.hpp"
#include "http/http2_config.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "url.hpp"
#include "util/defer.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;

using namespace std::chrono_literals;
using net::coro::task;

namespace
{

// How much a connection's window starts with: the plug's body.
constexpr std::size_t initial_window = 65'535;

constexpr std::size_t frame = 16'384; // the server's MAX_FRAME_SIZE: the client's DATA frames' most

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// held_config is the test's server's session: streams' windows large, so only the connection's holds the client's DATA
// back, and that left at its initial 65535 (no WINDOW_UPDATE raising it).
http2::session_config held_config()
{
    http2::session_config config;
    config.local.initial_window_size = std::uint32_t{1} << 20;
    config.connection_window         = initial_window;
    return config;
}

// outcome is what a request sent in the background came to: its response's status, or the error.
struct outcome
{
    std::atomic<std::uint64_t> finished{0};
    int                        status = 0;
    std::error_code            err;

    // wait waits (up to 5 seconds) for the request to have finished, returning whether it has.
    task<bool> wait(net::io::scheduler* sched)
    {
        const auto deadline = net::io::clock::now() + 5s;
        while (finished.load() == 0)
        {
            if (co_await sched->wait_on(finished, 0, deadline)) break;
        }
        co_return finished.load() != 0;
    }
};

// send_request sends req on conn, recording what happens in out (the response's body isn't read).
task<> send_request(net::io::scheduler*                                    sched,
                    std::shared_ptr<http::detail::http2_client_connection> conn,
                    http::client_request                                   req,
                    outcome*                                               out)
{
    auto resp = co_await conn->send(req);
    if (resp.has_value()) out->status = static_cast<int>(resp->status_code);
    else out->err = resp.error();

    out->finished.store(1);
    sched->notify_all(out->finished);
}

// upload is a request with a body of size bytes (and its Content-Length), canceled by stop.
http::client_request upload(std::size_t size, std::stop_token stop = {})
{
    http::client_request req{.method = http::request_method::POST,
                             .uri    = net::url::parse("http://example.com/upload").value()};
    req.set_body(std::string(size, 'x'));
    req.stop = std::move(stop);
    return req;
}

// fake_server is an HTTP/2 client connection to a server the test plays.
struct fake_server
{
    net::tcp_socket                                        socket; // the server's side
    std::shared_ptr<http::detail::http2_client_connection> conn;
};

task<fake_server> connect_fake(net::io::scheduler* sched, const http::detail::http2_client_options& options)
{
    auto listener = net::listener::make(sched, "127.0.0.1", "0").value();
    auto client   = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(listener.local_port()));
    REQUIRE(client.has_value());
    auto accepted = co_await listener.accept();
    REQUIRE(accepted.has_value());

    auto conn = std::make_shared<http::detail::http2_client_connection>(
        sched,
        std::make_unique<http::detail::connection>(std::move(*client)),
        options,
        http::detail::http2_client_connection::callbacks{});
    REQUIRE(conn->start());
    co_return fake_server{.socket = std::move(*accepted), .conn = std::move(conn)};
}

// watch closes conn after 10 seconds, unless stop is requested first: so a wait for streams to queue (which has no
// deadline of its own) ends, and fails its test, if they never do.
task<>
watch(net::io::scheduler* sched, std::shared_ptr<http::detail::http2_client_connection> conn, std::stop_token stop)
{
    const auto err = co_await sched->sleep_until(net::io::clock::now() + 10s, std::move(stop));
    if (!err) conn->close();
}

// held is the test's server, holding the connection's window shut (see the top), and what the bodies under test are.
struct held
{
    held(net::io::scheduler* sched, fake_server* fake, const http2::session_config& config = held_config())
        : sched{sched}
        , fake{fake}
        , server{&fake->socket, &fake->socket, &fake->socket, http2::role::server, config}
    {
        server.hold_windows = true;
        REQUIRE(sched->spawn(watch(sched, fake->conn, watching.get_token())));
    }

    held(const held&)            = delete;
    held& operator=(const held&) = delete;
    held(held&&)                 = delete;
    held& operator=(held&&)      = delete;
    ~held() { watching.request_stop(); }

    net::io::scheduler* sched;
    fake_server*        fake;
    test::h2_client     server;
    std::stop_source    watching; // stops watch()

    std::vector<http2::stream_id> ids;             // the bodies under test's streams, in the order they were sent
    std::size_t                   plug_frames = 0; // the plug's DATA frames, first in server.data_log

    // plug sends the plug's request (recording it in out), and waits until its body's all arrived, returning whether
    // it has: then the client has no window left.
    task<bool> plug(outcome* out)
    {
        const bool spawned = sched->spawn(send_request(sched, fake->conn, upload(initial_window), out));
        if (!spawned) co_return false;
        const bool plugged = co_await server.wait_for(1);
        if (!plugged) co_return false;
        plug_frames = server.data_log.size();
        co_return server.responses[1].body.size() == initial_window;
    }

    // send sends req (recording it in out) as the next stream, and waits until its body's waiting in the scheduler
    // (with as many before it as were sent before it), and the server has its head, returning whether both are so.
    task<bool> send(http::client_request req, outcome* out)
    {
        const auto id = static_cast<http2::stream_id>(3 + (2 * ids.size()));
        ids.push_back(id);
        const bool spawned = sched->spawn(send_request(sched, fake->conn, std::move(req), out));
        if (!spawned) co_return false;
        const bool queued = co_await fake->conn->wait_queued(ids.size());
        if (!queued) co_return false;
        co_return co_await server.pump([this, id] { return server.responses.contains(id); });
    }

    // give lets the client send n more bytes on the connection, in one WINDOW_UPDATE: as the plug's consumed (whose
    // stream has ended, so only the connection's window is given back).
    task<bool> give(std::size_t n)
    {
        server.s.consume(1, n);
        return server.pump([] { return true; }); // (what's to be written is, first)
    }

    // received waits until the server's received n DATA frames since the plug's.
    task<bool> received(std::size_t n)
    {
        return server.pump([this, n] { return server.data_log.size() >= plug_frames + n; });
    }

    // sent is the DATA frames received since the plug's.
    [[nodiscard]] std::span<const std::pair<http2::stream_id, std::size_t>> sent() const
    {
        return std::span{server.data_log}.subspan(plug_frames);
    }

    // frames is the DATA frames received since the plug's, as their streams' letters (A for ids' first, ...).
    [[nodiscard]] std::string frames() const
    {
        std::string out;
        for (const auto& [id, _] : sent())
        {
            const auto it = std::ranges::find(ids, id);
            out += it == ids.end() ? '?' : static_cast<char>('A' + (it - ids.begin()));
        }
        return out;
    }

    // body_size is how much of a stream's body has arrived.
    [[nodiscard]] std::size_t body_size(http2::stream_id id) { return server.responses[id].body.size(); }
};

}

TEST_CASE("the client's HTTP/2 request bodies that contend take turns, a frame each",
          "[http][http2][client][scheduler]")
{
    std::array<outcome, 4> outcomes; // (before the scheduler, which waits for the requests)
    net::io::scheduler     sched{make_pool()};

    const auto test = [](net::io::scheduler* sched, std::array<outcome, 4>* outcomes) -> task<>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        held             h{sched, &fake};

        const bool plugged = co_await h.plug(&outcomes->at(0));
        REQUIRE(plugged);

        // three bodies of three frames each, queued in order (each sent once the one before has queued).
        for (std::size_t i = 1; i < outcomes->size(); ++i)
        {
            const bool queued = co_await h.send(upload(3 * frame), &outcomes->at(i));
            REQUIRE(queued);
        }

        // a frame's worth of window at a time, each given once every body with frames left is waiting again: so each
        // frame's the scheduler's pick of all of them.
        std::map<http2::stream_id, int> left{
            {h.ids[0], 3},
            {h.ids[1], 3},
            {h.ids[2], 3}
        };
        for (std::size_t n = 0; n < 9; ++n)
        {
            const auto waiting =
                static_cast<std::size_t>(std::ranges::count_if(left, [](const auto& e) { return e.second > 0; }));
            const bool queued = co_await fake.conn->wait_queued(waiting);
            REQUIRE(queued);

            const bool given = co_await h.give(frame);
            REQUIRE(given);
            const bool arrived = co_await h.received(n + 1);
            REQUIRE(arrived);
            --left[h.server.data_log.back().first];
        }

        // round-robin, in the order they queued, each frame whole.
        CHECK(h.frames() == "ABCABCABC");
        for (const auto& [_, size] : h.sent()) CHECK(size == frame);
        for (const auto id : h.ids)
        {
            CHECK(h.body_size(id) == 3 * frame);
            CHECK(h.server.responses[id].ended_by_data == frame); // (a Content-Length: the last DATA ends it)
        }
    };

    sched.run_to_completion(test(&sched, &outcomes));
}

TEST_CASE("the client's HTTP/2 request bodies sent at once share the connection as its window opens",
          "[http][http2][client][scheduler]")
{
    std::array<outcome, 4> outcomes; // (before the scheduler, which waits for the requests)
    net::io::scheduler     sched{make_pool()};

    const auto test = [](net::io::scheduler* sched, std::array<outcome, 4>* outcomes) -> task<>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        held             h{sched, &fake};

        const bool plugged = co_await h.plug(&outcomes->at(0));
        REQUIRE(plugged);
        for (std::size_t i = 1; i < outcomes->size(); ++i)
        {
            const bool queued = co_await h.send(upload(4 * frame), &outcomes->at(i));
            REQUIRE(queued);
        }

        // the window for all of them at once: the first round is a frame each, in the order they queued. (Later
        // rounds have each body whose next write has queued by then: each write is a frame, read 16 KiB at a time, so
        // a body whose task comes back first may go twice in a row.)
        const bool given = co_await h.give(12 * frame);
        REQUIRE(given);
        const bool arrived = co_await h.received(12);
        REQUIRE(arrived);

        const auto order = h.frames();
        CAPTURE(order);
        CHECK(order.starts_with("ABC"));
        for (const auto id : h.ids)
        {
            CHECK(h.body_size(id) == 4 * frame);
            CHECK(h.server.responses[id].ended_by_data == frame);
        }
    };

    sched.run_to_completion(test(&sched, &outcomes));
}

TEST_CASE("an HTTP/2 request body alone goes in whole frames, through the scheduler too",
          "[http][http2][client][scheduler]")
{
    std::array<outcome, 2> outcomes; // (before the scheduler, which waits for the requests)
    net::io::scheduler     sched{make_pool()};

    const auto test = [](net::io::scheduler* sched, std::array<outcome, 2>* outcomes) -> task<>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        held             h{sched, &fake};

        const bool plugged = co_await h.plug(&outcomes->at(0));
        REQUIRE(plugged);
        const bool queued = co_await h.send(upload((4 * frame) + 100), &outcomes->at(1));
        REQUIRE(queued);

        // its first frame from the scheduler, the rest at once (nothing contends), all whole.
        const bool given = co_await h.give((4 * frame) + 100);
        REQUIRE(given);
        const bool arrived = co_await h.received(5);
        REQUIRE(arrived);

        CHECK(h.frames() == "AAAAA");
        std::vector<std::size_t> sizes;
        for (const auto& [_, size] : h.sent()) sizes.push_back(size);
        CHECK(sizes == std::vector<std::size_t>{frame, frame, frame, frame, 100});
        CHECK(h.server.responses[h.ids[0]].ended_by_data == 100);
    };

    sched.run_to_completion(test(&sched, &outcomes));
}

TEST_CASE("an HTTP/2 request body waiting for its turn times out with io_timeout", "[http][http2][client][scheduler]")
{
    std::array<outcome, 2> outcomes; // (before the scheduler, which waits for the requests)
    net::io::scheduler     sched{make_pool()};

    const auto test = [](net::io::scheduler* sched, std::array<outcome, 2>* outcomes) -> task<>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 300ms});
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        held             h{sched, &fake};

        const bool plugged = co_await h.plug(&outcomes->at(0));
        REQUIRE(plugged);
        const bool queued = co_await h.send(upload(2 * frame), &outcomes->at(1));
        REQUIRE(queued);

        // the window never opens: the stream's reset, and the request fails.
        const auto id    = h.ids[0];
        const bool reset = co_await h.server.pump([&] { return static_cast<bool>(h.server.responses[id].reset); });
        REQUIRE(reset);
        CHECK(h.server.responses[id].reset == http2::error_code::cancel);
        CHECK(h.body_size(id) == 0);

        const bool finished = co_await (*outcomes)[1].wait(sched);
        REQUIRE(finished);
        CHECK((*outcomes)[1].err == std::errc::timed_out);

        // and the connection goes on: the plug's answered.
        h.server.respond(1, "204");
        const bool written = co_await h.server.pump([] { return true; });
        REQUIRE(written);
        const bool answered = co_await (*outcomes)[0].wait(sched);
        REQUIRE(answered);
        CHECK((*outcomes)[0].status == 204);
    };

    sched.run_to_completion(test(&sched, &outcomes));
}

TEST_CASE("an HTTP/2 request canceled while its body waits for its turn leaves the others to go on",
          "[http][http2][client][scheduler][cancel]")
{
    std::array<outcome, 3> outcomes; // (before the scheduler, which waits for the requests)
    net::io::scheduler     sched{make_pool()};

    const auto test = [](net::io::scheduler* sched, std::array<outcome, 3>* outcomes) -> task<>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        held             h{sched, &fake};

        const bool plugged = co_await h.plug(&outcomes->at(0));
        REQUIRE(plugged);

        std::stop_source source;
        bool             queued = co_await h.send(upload(3 * frame, source.get_token()), &outcomes->at(1));
        REQUIRE(queued);
        queued = co_await h.send(upload(3 * frame), &outcomes->at(2));
        REQUIRE(queued);

        // the first's turn, with less than a frame's window: it's framed that much of its write, which waits on for
        // the rest.
        const auto first  = h.ids[0];
        const auto second = h.ids[1];
        const bool given  = co_await h.give(10'000);
        REQUIRE(given);
        const bool arrived = co_await h.received(1);
        REQUIRE(arrived);
        CHECK(h.body_size(first) == 10'000);
        queued = co_await fake.conn->wait_queued(2);
        REQUIRE(queued);

        // canceled there: its stream's reset, its write returns what was framed, and the request fails.
        source.request_stop();
        const bool reset = co_await h.server.pump([&] { return static_cast<bool>(h.server.responses[first].reset); });
        REQUIRE(reset);
        CHECK(h.server.responses[first].reset == http2::error_code::cancel);
        const bool finished = co_await (*outcomes)[1].wait(sched);
        REQUIRE(finished);
        CHECK((*outcomes)[1].err == std::errc::operation_canceled);

        // the other goes on, whole, as the window opens.
        const bool rest = co_await h.give(3 * frame);
        REQUIRE(rest);
        const bool done = co_await h.server.wait_for(second);
        REQUIRE(done);
        CHECK(h.body_size(second) == 3 * frame);
        CHECK(h.body_size(first) == 10'000);
        CHECK_FALSE(h.server.responses[second].reset);

        h.server.respond(second, "204");
        const bool written = co_await h.server.pump([] { return true; });
        REQUIRE(written);
        const bool answered = co_await (*outcomes)[2].wait(sched);
        REQUIRE(answered);
        CHECK((*outcomes)[2].status == 204);
    };

    sched.run_to_completion(test(&sched, &outcomes));
}

TEST_CASE("an HTTP/2 request whose body waits its turn gets the server's response, and reset, before the body",
          "[http][http2][client][scheduler]")
{
    std::array<outcome, 3> outcomes; // (before the scheduler, which waits for the requests)
    net::io::scheduler     sched{make_pool()};

    const auto test = [](net::io::scheduler* sched, std::array<outcome, 3>* outcomes) -> task<>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        held             h{sched, &fake};

        const bool plugged = co_await h.plug(&outcomes->at(0));
        REQUIRE(plugged);
        bool queued = co_await h.send(upload(3 * frame), &outcomes->at(1));
        REQUIRE(queued);
        queued = co_await h.send(upload(3 * frame), &outcomes->at(2));
        REQUIRE(queued);

        // 413 before any of the first's body, as a server does that won't read it: the stream's done.
        const auto first  = h.ids[0];
        const auto second = h.ids[1];
        h.server.respond(first, "413");
        h.server.s.reset(first, http2::error_code::no_error);
        const bool written = co_await h.server.pump([] { return true; });
        REQUIRE(written);

        const bool finished = co_await (*outcomes)[1].wait(sched);
        REQUIRE(finished);
        CHECK((*outcomes)[1].status == 413);
        CHECK_FALSE((*outcomes)[1].err);

        // its body never goes; the other's does, whole, as the window opens.
        const bool given = co_await h.give(3 * frame);
        REQUIRE(given);
        const bool done = co_await h.server.wait_for(second);
        REQUIRE(done);
        CHECK(h.body_size(second) == 3 * frame);
        CHECK(h.body_size(first) == 0);
        CHECK(h.frames() == "BBB");
    };

    sched.run_to_completion(test(&sched, &outcomes));
}

TEST_CASE("an HTTP/2 request body without its stream's window doesn't hold up another's",
          "[http][http2][client][scheduler]")
{
    std::array<outcome, 2> outcomes; // (before the scheduler, which waits for the requests)
    net::io::scheduler     sched{make_pool()};

    const auto test = [](net::io::scheduler* sched, std::array<outcome, 2>* outcomes) -> task<>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};

        // streams' windows of 65535, never given back; the connection's open (1 MiB).
        held h{sched, &fake, http2::session_config{}};

        // the first sends its stream's window's worth at once (nothing contends), and waits for the rest.
        const auto first   = static_cast<http2::stream_id>(1);
        const bool spawned = sched->spawn(send_request(sched, fake.conn, upload(5 * frame), &outcomes->at(0)));
        REQUIRE(spawned);
        bool queued = co_await fake.conn->wait_queued(1);
        REQUIRE(queued);
        const bool windowed = co_await h.server.pump([&] { return h.body_size(first) == initial_window; });
        REQUIRE(windowed);

        // the second goes past it, whole, though it's queued behind it.
        queued = co_await h.send(upload(2 * frame), &outcomes->at(1));
        REQUIRE(queued);
        const auto second = h.ids[0];
        const bool done   = co_await h.server.wait_for(second);
        REQUIRE(done);
        CHECK(h.body_size(second) == 2 * frame);
        CHECK(h.body_size(first) == initial_window);

        // then the first, as its window grows.
        h.server.s.consume(first, initial_window);
        const bool rest = co_await h.server.wait_for(first);
        REQUIRE(rest);
        CHECK(h.body_size(first) == 5 * frame);
    };

    sched.run_to_completion(test(&sched, &outcomes));
}
