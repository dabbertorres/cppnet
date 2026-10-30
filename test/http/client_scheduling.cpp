// The client's HTTP/2 request bodies through its connection's scheduler: a body's write goes at once while nothing
// contends, and otherwise waits in the scheduler for its turn, by its request's Priority field (read as RFC 9218 says;
// u=3 incremental without one), so bodies sent at once without one share the connection a frame each.
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
#include <initializer_list>
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

#include <sys/socket.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/connection.hpp"
#include "http/h2_client.hpp"
#include "http/http2.hpp"
#include "http/http2_client.hpp"
#include "http/http2_config.hpp"
#include "http/http2_session.hpp"
#include "http/priority.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "url.hpp"
#include "util/defer.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::writer_gate;

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

// prioritized is an upload of size bytes whose request has a Priority field of these lines (one field line each).
http::client_request prioritized(std::size_t size, std::initializer_list<std::string_view> lines)
{
    auto req = upload(size);
    for (const auto line : lines) req.headers.add("Priority", line);
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

    // take_turns gives the window a frame's worth at a time, each once every body under test with frames left (each
    // of frames_each frames) is waiting again: so each frame's the scheduler's pick of all of them. It returns whether
    // they all went.
    task<bool> take_turns(std::size_t frames_each)
    {
        std::map<http2::stream_id, std::size_t> left;
        for (const auto id : ids) left[id] = frames_each;

        for (std::size_t n = 0; n < frames_each * ids.size(); ++n)
        {
            const auto waiting =
                static_cast<std::size_t>(std::ranges::count_if(left, [](const auto& e) { return e.second > 0; }));
            const bool queued = co_await fake->conn->wait_queued(waiting);
            if (!queued) co_return false;

            const bool given = co_await give(frame);
            if (!given) co_return false;
            const bool arrived = co_await received(n + 1);
            if (!arrived) co_return false;

            auto& frames_left = left[server.data_log.back().first];
            if (frames_left == 0) co_return false; // (a stream's frame past its body, or another's)
            --frames_left;
        }
        co_return true;
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
        const bool went = co_await h.take_turns(3);
        REQUIRE(went);

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

TEST_CASE("the client's HTTP/2 request bodies take their turns by their requests' Priority fields",
          "[http][http2][client][scheduler][priority]")
{
    std::array<outcome, 7> outcomes; // (before the scheduler, which waits for the requests)
    net::io::scheduler     sched{make_pool()};

    const auto test = [](net::io::scheduler* sched, std::array<outcome, 7>* outcomes) -> task<>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};
        held             h{sched, &fake};

        const bool plugged = co_await h.plug(&outcomes->at(0));
        REQUIRE(plugged);

        // two frames each, queued in order. A field is read as RFC 9218 says (what it leaves out is u=3, not
        // incremental: B's and C's), and E's is two field lines, read as one value; D, without one, is u=3
        // incremental.
        std::array<http::client_request, 6> requests{
            prioritized(2 * frame, {"u=5, i"}),   // A
            prioritized(2 * frame, {"u=1"}),      // B
            prioritized(2 * frame, {"u=1"}),      // C
            upload(2 * frame),                    // D: none
            prioritized(2 * frame, {"i", "u=5"}), // E
            prioritized(2 * frame, {"u=0"}),      // F
        };
        for (std::size_t i = 0; i < requests.size(); ++i)
        {
            const bool queued = co_await h.send(std::move(requests.at(i)), &outcomes->at(i + 1));
            REQUIRE(queued);
        }

        const bool went = co_await h.take_turns(2);
        REQUIRE(went);

        // the most urgent first; of one urgency, the non-incremental ones a body at a time by stream, the incremental
        // ones a frame each.
        CHECK(h.frames() == "FFBBCCDDAEAE");
        for (const auto id : h.ids) CHECK(h.body_size(id) == 2 * frame);
    };

    sched.run_to_completion(test(&sched, &outcomes));
}

TEST_CASE("an HTTP/2 request body whose Priority field doesn't parse takes RFC 9218's default's turns",
          "[http][http2][client][scheduler][priority]")
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

        // A's field isn't a Dictionary (a key can't start with '@'), so it's RFC 9218's default whole, u=3 not
        // incremental (not u=0); B, without a field, is u=3 incremental: of one urgency, the two kinds take turns,
        // incremental first.
        std::array<http::client_request, 3> requests{
            prioritized(2 * frame, {"u=0, @"}), // A
            upload(2 * frame),                  // B
            prioritized(2 * frame, {"u=4"}),    // C
        };
        for (std::size_t i = 0; i < requests.size(); ++i)
        {
            const bool queued = co_await h.send(std::move(requests.at(i)), &outcomes->at(i + 1));
            REQUIRE(queued);
        }

        const bool went = co_await h.take_turns(2);
        REQUIRE(went);
        CHECK(h.frames() == "BABACC");
    };

    sched.run_to_completion(test(&sched, &outcomes));
}

TEST_CASE("an HTTP/2 request reprioritized while its body waits takes its new turns, and tells the server",
          "[http][http2][client][scheduler][priority]")
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

        // three bodies without fields, u=3 incremental, which would take turns ("ABCABC").
        for (std::size_t i = 0; i < 3; ++i)
        {
            const bool queued = co_await h.send(upload(2 * frame), &outcomes->at(i + 1));
            REQUIRE(queued);
        }

        // reprioritized as they wait (as client_response::set_priority() does, through the response's body): C first,
        // whole, and A after B; and the server's told. (A stream the connection doesn't have can't be.)
        CHECK(fake.conn->reprioritize(h.ids.at(2), {.urgency = 0}));
        CHECK(fake.conn->reprioritize(h.ids.at(0), {.urgency = 7}));
        CHECK_FALSE(fake.conn->reprioritize(99, {.urgency = 1}));

        const bool went = co_await h.take_turns(2);
        REQUIRE(went);
        CHECK(h.frames() == "CCBBAA");

        const bool told = co_await h.server.pump([&h] { return h.server.priority_updates.size() >= 2; });
        REQUIRE(told);
        const std::vector<std::pair<http2::stream_id, http::priority>> want{
            {h.ids.at(2), {.urgency = 0}},
            {h.ids.at(0), {.urgency = 7}},
        };
        CHECK(h.server.priority_updates == want);

        // once the connection's ended, failing the streams, none can be (though the session knows nothing of it).
        fake.conn->close();
        CHECK_FALSE(fake.conn->reprioritize(h.ids.at(1), {.urgency = 1}));
    };

    sched.run_to_completion(test(&sched, &outcomes));
}

// TCP_NOTSENT_LOWAT on the client's HTTP/2 connections, as on the server's: set on each HTTP/2 connection's socket (not
// on HTTP/1.1's), and the writer waiting for the socket to take more before each round, so that the bodies it frames
// are in the scheduler's order up to the last moment.

TEST_CASE("the client sets TCP_NOTSENT_LOWAT on its HTTP/2 connections, not on HTTP/1.1's",
          "[http][http2][client][notsent-lowat]")
{
    struct test_case
    {
        std::string_view           name;
        bool                       http2 = true;
        std::optional<std::size_t> mark{}; // the client's http2_notsent_lowat, if not the default
        std::string_view           want;
    };

    const auto tc = GENERATE(test_case{.name = "HTTP/2, by default", .want = "2: 16384"},
                             test_case{.name = "HTTP/2, a mark of its own", .mark = 4'096, .want = "2: 4096"},
                             test_case{.name = "HTTP/2, none", .mark = 0, .want = "2: 0"},
                             test_case{.name = "HTTP/1.1", .http2 = false, .want = "1: 0"});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};

    http::router r;
    r.GET("/",
          [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          {
              auto* body = co_await resp.send(http::status::OK, 5);
              (void)co_await net::io::write_all(*body, "hello"sv);
          });
    // (the server's own mark off, though it's on another socket.)
    http::server server{&sched,
                        net::listener::make(&sched, "127.0.0.1", "0").value(),
                        std::move(r),
                        {.http2_notsent_lowat = 0}};
    REQUIRE(sched.spawn(server.serve()));

    // HTTP/2 in cleartext with prior knowledge (its preface), HTTP/1.1 otherwise.
    http::client_options options;
    options.http2                 = tc.http2;
    options.http2_prior_knowledge = tc.http2;
    if (tc.mark.has_value()) options.http2_notsent_lowat = *tc.mark;
    http::client client{&sched, options};

    // the mark on the client's socket (the response's body's handle, before the body's read, which gives an
    // HTTP/1.1 connection back), and the version.
    const auto fetch = [](http::client* client, std::string url) -> task<std::string>
    {
        auto resp = co_await client->get(url);
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        int        value = -1;
        socklen_t  size  = sizeof(value);
        const bool got = ::getsockopt(resp->body->native_handle(), IPPROTO_TCP, TCP_NOTSENT_LOWAT, &value, &size) == 0;

        auto body = co_await net::io::read_all(*resp->body);
        if (!body.has_value()) co_return "error: " + body.error().message();
        if (*body != "hello") co_return "body: " + *body;
        co_return std::to_string(resp->version.major) + ": " + (got ? std::to_string(value) : "(getsockopt failed)");
    };

    const auto out = sched.run_to_completion(fetch(&client, "http://127.0.0.1:" + std::to_string(server.local_port())));
    CHECK(out == tc.want);
}

TEST_CASE("the client's HTTP/2 writer, held by the socket, frames the bodies that queue meanwhile by priority",
          "[http][http2][client][scheduler][notsent-lowat]")
{
    std::array<outcome, 4> outcomes; // (before the scheduler, which waits for the requests)
    writer_gate            gate;     // (and the connection's writer)
    net::io::scheduler     sched{make_pool()};
    gate.sched = &sched;

    const auto test = [](net::io::scheduler* sched, std::array<outcome, 4>* outcomes, writer_gate* gate) -> task<>
    {
        // the mark set (on the real socket), so the writer waits, through the gate.
        auto fake = co_await connect_fake(
            sched,
            {.io_timeout = 10s, .notsent_lowat = std::size_t{16} * 1'024, .writable = gate->hook()});
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake.conn->close(); }};

        // the server's windows open (16 MiB), and given back as DATA arrives: only the writer holds the bodies back.
        http2::session_config config;
        config.local.initial_window_size = std::uint32_t{16} << 20;
        config.connection_window         = std::uint32_t{16} << 20;
        test::h2_client server{&fake.socket, &fake.socket, &fake.socket, http2::role::server, config};

        std::stop_source watching;
        REQUIRE(sched->spawn(watch(sched, fake.conn, watching.get_token())));
        net::util::defer unwatch{[&watching] noexcept { watching.request_stop(); }};

        // a request first, so the settings have been exchanged (the server's windows are the client's to use).
        http::client_request get{.method = http::request_method::GET,
                                 .uri    = net::url::parse("http://example.com/").value()};
        const bool           spawned = sched->spawn(send_request(sched, fake.conn, std::move(get), &outcomes->at(0)));
        REQUIRE(spawned);
        const bool asked = co_await server.pump([&server] { return server.responses.contains(1); });
        REQUIRE(asked);
        server.respond(1, "204");
        const bool answered = co_await server.pump([] { return true; });
        REQUIRE(answered);
        const bool finished = co_await (*outcomes)[0].wait(sched);
        REQUIRE(finished);

        // held from now on: a u=7 body of 8 frames goes at once as far as max_ahead (64 KiB, 4 frames), and queues
        // the rest; then a u=0 and a u=3 body of a frame each queue behind it.
        gate->hold();
        std::array<http::client_request, 3> requests{
            prioritized(8 * frame, {"u=7"}), // B (bulk), stream 3
            prioritized(frame, {"u=0"}),     // U (urgent), stream 5
            prioritized(frame, {"u=3"}),     // N (normal), stream 7
        };
        for (std::size_t i = 0; i < requests.size(); ++i)
        {
            const bool sending =
                sched->spawn(send_request(sched, fake.conn, std::move(requests.at(i)), &outcomes->at(i + 1)));
            REQUIRE(sending);
            const bool queued = co_await fake.conn->wait_queued(i + 1);
            REQUIRE(queued);
        }

        // let go: B's first 4 frames (framed already), then U's, N's, and the rest of B's.
        gate->open();
        for (const http2::stream_id id : {3, 5, 7})
        {
            const bool done = co_await server.wait_for(id);
            REQUIRE(done);
        }

        const std::map<http2::stream_id, char> names{
            {3, 'B'},
            {5, 'U'},
            {7, 'N'}
        };
        std::string order;
        for (const auto& [id, _] : server.data_log) order += names.contains(id) ? names.at(id) : '?';
        CHECK(order == "BBBBUNBBBB");
        CHECK(server.responses[3].body.size() == 8 * frame);
        CHECK(server.responses[5].body.size() == frame);
        CHECK(server.responses[7].body.size() == frame);
    };

    sched.run_to_completion(test(&sched, &outcomes, &gate));
}
