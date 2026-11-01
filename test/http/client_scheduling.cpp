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
#include <format>
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

// connect_fake connects an HTTP/2 client connection to a listener of its own, returning it (and the server's side), or
// what went wrong.
task<std::expected<fake_server, std::string>> connect_fake(net::io::scheduler*                       sched,
                                                           const http::detail::http2_client_options& options)
{
    auto listener = net::listener::make(sched, "127.0.0.1", "0").value();
    auto client   = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(listener.local_port()));
    if (!client.has_value()) co_return std::unexpected("not connected: " + client.error().message());
    auto accepted = co_await listener.accept();
    if (!accepted.has_value()) co_return std::unexpected("not accepted: " + accepted.error().message());

    auto conn = std::make_shared<http::detail::http2_client_connection>(
        sched,
        std::make_unique<http::detail::connection>(std::move(*client)),
        options,
        http::detail::http2_client_connection::callbacks{});
    if (!conn->start()) co_return std::unexpected("the connection didn't start");
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
        watched             = sched->spawn(watch(sched, fake->conn, watching.get_token()));
    }

    held(const held&)            = delete;
    held& operator=(const held&) = delete;
    held(held&&)                 = delete;
    held& operator=(held&&)      = delete;
    ~held() { watching.request_stop(); }

    net::io::scheduler* sched;
    fake_server*        fake;
    test::h2_client     server;
    std::stop_source    watching;        // stops watch()
    bool                watched = false; // whether watch() was spawned (the test fails if it wasn't)

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
    // (before the scheduler, which waits for the requests)
    std::array<outcome, 4>                                outcomes;
    std::string                                           order;
    std::vector<std::pair<http2::stream_id, std::size_t>> sent;   // the DATA frames since the plug's
    std::vector<test::h2_client::response>                bodies; // the bodies under test, in the order they were sent
    net::io::scheduler                                    sched{make_pool()};

    const auto test = [](net::io::scheduler*                                    sched,
                         std::array<outcome, 4>*                                outcomes,
                         std::string*                                           order,
                         std::vector<std::pair<http2::stream_id, std::size_t>>* sent,
                         std::vector<test::h2_client::response>*                bodies) -> task<std::string>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        if (!fake.has_value()) co_return fake.error();
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake->conn->close(); }};
        held             h{sched, &*fake};
        if (!h.watched) co_return "the watch wasn't spawned";

        const bool plugged = co_await h.plug(&outcomes->at(0));
        if (!plugged) co_return "not plugged";

        // three bodies of three frames each, queued in order (each sent once the one before has queued).
        for (std::size_t i = 1; i < outcomes->size(); ++i)
        {
            const bool queued = co_await h.send(upload(3 * frame), &outcomes->at(i));
            if (!queued) co_return std::format("body {} not queued", i);
        }

        // a frame's worth of window at a time, each given once every body with frames left is waiting again: so each
        // frame's the scheduler's pick of all of them.
        const bool went = co_await h.take_turns(3);
        if (!went) co_return "not all went: " + h.frames();

        *order = h.frames();
        sent->assign(h.sent().begin(), h.sent().end());
        for (const auto id : h.ids) bodies->push_back(h.server.responses[id]);
        co_return h.server.failure;
    };

    const auto failed = sched.run_to_completion(test(&sched, &outcomes, &order, &sent, &bodies));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    // round-robin, in the order they queued, each frame whole.
    CHECK(order == "ABCABCABC");
    for (const auto& [_, size] : sent) CHECK(size == frame);
    for (const auto& r : bodies)
    {
        CHECK(r.body.size() == 3 * frame);
        CHECK(r.ended_by_data == frame); // (a Content-Length: the last DATA ends it)
    }
}

TEST_CASE("the client's HTTP/2 request bodies sent at once share the connection as its window opens",
          "[http][http2][client][scheduler]")
{
    // (before the scheduler, which waits for the requests)
    std::array<outcome, 4>                 outcomes;
    std::string                            order;
    std::vector<test::h2_client::response> bodies; // the bodies under test, in the order they were sent
    net::io::scheduler                     sched{make_pool()};

    const auto test = [](net::io::scheduler*                     sched,
                         std::array<outcome, 4>*                 outcomes,
                         std::string*                            order,
                         std::vector<test::h2_client::response>* bodies) -> task<std::string>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        if (!fake.has_value()) co_return fake.error();
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake->conn->close(); }};
        held             h{sched, &*fake};
        if (!h.watched) co_return "the watch wasn't spawned";

        const bool plugged = co_await h.plug(&outcomes->at(0));
        if (!plugged) co_return "not plugged";
        for (std::size_t i = 1; i < outcomes->size(); ++i)
        {
            const bool queued = co_await h.send(upload(4 * frame), &outcomes->at(i));
            if (!queued) co_return std::format("body {} not queued", i);
        }

        // the window for all of them at once: the first round is a frame each, in the order they queued. (Later
        // rounds have each body whose next write has queued by then: each write is a frame, read 16 KiB at a time, so
        // a body whose task comes back first may go twice in a row.)
        const bool given = co_await h.give(12 * frame);
        if (!given) co_return "the window not given";
        const bool arrived = co_await h.received(12);
        if (!arrived) co_return "not all arrived: " + h.frames();

        *order = h.frames();
        for (const auto id : h.ids) bodies->push_back(h.server.responses[id]);
        co_return h.server.failure;
    };

    const auto failed = sched.run_to_completion(test(&sched, &outcomes, &order, &bodies));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CAPTURE(order);
    CHECK(order.starts_with("ABC"));
    for (const auto& r : bodies)
    {
        CHECK(r.body.size() == 4 * frame);
        CHECK(r.ended_by_data == frame);
    }
}

TEST_CASE("an HTTP/2 request body alone goes in whole frames, through the scheduler too",
          "[http][http2][client][scheduler]")
{
    // (before the scheduler, which waits for the requests)
    std::array<outcome, 2>     outcomes;
    std::string                order;
    std::vector<std::size_t>   sizes; // the DATA frames' since the plug's
    std::optional<std::size_t> ended_by_data;
    net::io::scheduler         sched{make_pool()};

    const auto test = [](net::io::scheduler*         sched,
                         std::array<outcome, 2>*     outcomes,
                         std::string*                order,
                         std::vector<std::size_t>*   sizes,
                         std::optional<std::size_t>* ended_by_data) -> task<std::string>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        if (!fake.has_value()) co_return fake.error();
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake->conn->close(); }};
        held             h{sched, &*fake};
        if (!h.watched) co_return "the watch wasn't spawned";

        const bool plugged = co_await h.plug(&outcomes->at(0));
        if (!plugged) co_return "not plugged";
        const bool queued = co_await h.send(upload((4 * frame) + 100), &outcomes->at(1));
        if (!queued) co_return "not queued";

        // its first frame from the scheduler, the rest at once (nothing contends), all whole.
        const bool given = co_await h.give((4 * frame) + 100);
        if (!given) co_return "the window not given";
        const bool arrived = co_await h.received(5);
        if (!arrived) co_return "not all arrived: " + h.frames();

        *order = h.frames();
        for (const auto& [_, size] : h.sent()) sizes->push_back(size);
        *ended_by_data = h.server.responses[h.ids[0]].ended_by_data;
        co_return h.server.failure;
    };

    const auto failed = sched.run_to_completion(test(&sched, &outcomes, &order, &sizes, &ended_by_data));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(order == "AAAAA");
    CHECK(sizes == std::vector<std::size_t>{frame, frame, frame, frame, 100});
    CHECK(ended_by_data == 100U);
}

TEST_CASE("an HTTP/2 request body waiting for its turn times out with io_timeout", "[http][http2][client][scheduler]")
{
    // (before the scheduler, which waits for the requests)
    std::array<outcome, 2> outcomes;
    std::error_code        reset;
    std::size_t            received = 0; // of the body's
    net::io::scheduler     sched{make_pool()};

    const auto test = [](net::io::scheduler*     sched,
                         std::array<outcome, 2>* outcomes,
                         std::error_code*        reset,
                         std::size_t*            received) -> task<std::string>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 300ms});
        if (!fake.has_value()) co_return fake.error();
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake->conn->close(); }};
        held             h{sched, &*fake};
        if (!h.watched) co_return "the watch wasn't spawned";

        const bool plugged = co_await h.plug(&outcomes->at(0));
        if (!plugged) co_return "not plugged";
        const bool queued = co_await h.send(upload(2 * frame), &outcomes->at(1));
        if (!queued) co_return "not queued";

        // the window never opens: the stream's reset, and the request fails.
        const auto id        = h.ids[0];
        const bool was_reset = co_await h.server.pump([&] { return static_cast<bool>(h.server.responses[id].reset); });
        if (!was_reset) co_return "not reset";
        *reset    = h.server.responses[id].reset;
        *received = h.body_size(id);

        const bool finished = co_await (*outcomes)[1].wait(sched);
        if (!finished) co_return "the request didn't finish";

        // and the connection goes on: the plug's answered.
        h.server.respond(1, "204");
        const bool written = co_await h.server.pump([] { return true; });
        if (!written) co_return h.server.failure.empty() ? "the response wasn't written" : h.server.failure;
        const bool answered = co_await (*outcomes)[0].wait(sched);
        if (!answered) co_return "the plug wasn't answered";
        co_return h.server.failure;
    };

    const auto failed = sched.run_to_completion(test(&sched, &outcomes, &reset, &received));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(reset == http2::error_code::cancel);
    CHECK(received == 0);
    CHECK(outcomes[1].err == std::errc::timed_out);
    CHECK(outcomes[0].status == 204);
}

TEST_CASE("an HTTP/2 request canceled while its body waits for its turn leaves the others to go on",
          "[http][http2][client][scheduler][cancel]")
{
    // (before the scheduler, which waits for the requests)
    std::array<outcome, 3> outcomes;
    std::stop_source       source;           // the first's
    std::size_t            first_framed = 0; // of the first's body, once its turn came
    std::size_t            first_body   = 0; // and at the end
    std::error_code        first_reset;
    std::size_t            second_body = 0;
    std::error_code        second_reset;
    net::io::scheduler     sched{make_pool()};

    const auto test = [&](net::io::scheduler* sched, std::array<outcome, 3>* outcomes) -> task<std::string>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        if (!fake.has_value()) co_return fake.error();
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake->conn->close(); }};
        held             h{sched, &*fake};
        if (!h.watched) co_return "the watch wasn't spawned";

        const bool plugged = co_await h.plug(&outcomes->at(0));
        if (!plugged) co_return "not plugged";

        bool queued = co_await h.send(upload(3 * frame, source.get_token()), &outcomes->at(1));
        if (!queued) co_return "the first not queued";
        queued = co_await h.send(upload(3 * frame), &outcomes->at(2));
        if (!queued) co_return "the second not queued";

        // the first's turn, with less than a frame's window: it's framed that much of its write, which waits on for
        // the rest.
        const auto first  = h.ids[0];
        const auto second = h.ids[1];
        const bool given  = co_await h.give(10'000);
        if (!given) co_return "the window not given";
        const bool arrived = co_await h.received(1);
        if (!arrived) co_return "the first's frame didn't come";
        first_framed = h.body_size(first);
        queued       = co_await fake->conn->wait_queued(2);
        if (!queued) co_return "not both queued again";

        // canceled there: its stream's reset, its write returns what was framed, and the request fails.
        source.request_stop();
        const bool reset = co_await h.server.pump([&] { return static_cast<bool>(h.server.responses[first].reset); });
        if (!reset) co_return "the first not reset";
        first_reset         = h.server.responses[first].reset;
        const bool finished = co_await (*outcomes)[1].wait(sched);
        if (!finished) co_return "the first didn't finish";

        // the other goes on, whole, as the window opens.
        const bool rest = co_await h.give(3 * frame);
        if (!rest) co_return "the rest's window not given";
        const bool done = co_await h.server.wait_for(second);
        if (!done) co_return "the second unfinished";
        second_body  = h.body_size(second);
        first_body   = h.body_size(first);
        second_reset = h.server.responses[second].reset;

        h.server.respond(second, "204");
        const bool written = co_await h.server.pump([] { return true; });
        if (!written) co_return h.server.failure.empty() ? "the response wasn't written" : h.server.failure;
        const bool answered = co_await (*outcomes)[2].wait(sched);
        if (!answered) co_return "the second wasn't answered";
        co_return h.server.failure;
    };

    const auto failed = sched.run_to_completion(test(&sched, &outcomes));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(first_framed == 10'000);
    CHECK(first_reset == http2::error_code::cancel);
    CHECK(outcomes[1].err == std::errc::operation_canceled);
    CHECK(second_body == 3 * frame);
    CHECK(first_body == 10'000);
    CHECK_FALSE(second_reset);
    CHECK(outcomes[2].status == 204);
}

TEST_CASE("an HTTP/2 request whose body waits its turn gets the server's response, and reset, before the body",
          "[http][http2][client][scheduler]")
{
    // (before the scheduler, which waits for the requests)
    std::array<outcome, 3> outcomes;
    std::size_t            first_body  = 0;
    std::size_t            second_body = 0;
    std::string            order;
    net::io::scheduler     sched{make_pool()};

    const auto test = [&](net::io::scheduler* sched, std::array<outcome, 3>* outcomes) -> task<std::string>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        if (!fake.has_value()) co_return fake.error();
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake->conn->close(); }};
        held             h{sched, &*fake};
        if (!h.watched) co_return "the watch wasn't spawned";

        const bool plugged = co_await h.plug(&outcomes->at(0));
        if (!plugged) co_return "not plugged";
        bool queued = co_await h.send(upload(3 * frame), &outcomes->at(1));
        if (!queued) co_return "the first not queued";
        queued = co_await h.send(upload(3 * frame), &outcomes->at(2));
        if (!queued) co_return "the second not queued";

        // 413 before any of the first's body, as a server does that won't read it: the stream's done.
        const auto first  = h.ids[0];
        const auto second = h.ids[1];
        h.server.respond(first, "413");
        h.server.s.reset(first, http2::error_code::no_error);
        const bool written = co_await h.server.pump([] { return true; });
        if (!written) co_return h.server.failure.empty() ? "the response wasn't written" : h.server.failure;

        const bool finished = co_await (*outcomes)[1].wait(sched);
        if (!finished) co_return "the first didn't finish";

        // its body never goes; the other's does, whole, as the window opens.
        const bool given = co_await h.give(3 * frame);
        if (!given) co_return "the window not given";
        const bool done = co_await h.server.wait_for(second);
        if (!done) co_return "the second unfinished";
        second_body = h.body_size(second);
        first_body  = h.body_size(first);
        order       = h.frames();
        co_return h.server.failure;
    };

    const auto failed = sched.run_to_completion(test(&sched, &outcomes));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(outcomes[1].status == 413);
    CHECK_FALSE(outcomes[1].err);
    CHECK(second_body == 3 * frame);
    CHECK(first_body == 0);
    CHECK(order == "BBB");
}

TEST_CASE("an HTTP/2 request body without its stream's window doesn't hold up another's",
          "[http][http2][client][scheduler]")
{
    // (before the scheduler, which waits for the requests)
    std::array<outcome, 2> outcomes;
    std::size_t            first_held  = 0; // of the first's body, while the second went
    std::size_t            first_body  = 0; // at the end
    std::size_t            second_body = 0;
    net::io::scheduler     sched{make_pool()};

    const auto test = [&](net::io::scheduler* sched, std::array<outcome, 2>* outcomes) -> task<std::string>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        if (!fake.has_value()) co_return fake.error();
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake->conn->close(); }};

        // streams' windows of 65535, never given back; the connection's open (1 MiB).
        held h{sched, &*fake, http2::session_config{}};
        if (!h.watched) co_return "the watch wasn't spawned";

        // the first sends its stream's window's worth at once (nothing contends), and waits for the rest.
        const auto first   = static_cast<http2::stream_id>(1);
        const bool spawned = sched->spawn(send_request(sched, fake->conn, upload(5 * frame), &outcomes->at(0)));
        if (!spawned) co_return "the first not sent";
        bool queued = co_await fake->conn->wait_queued(1);
        if (!queued) co_return "the first not queued";
        const bool windowed = co_await h.server.pump([&] { return h.body_size(first) == initial_window; });
        if (!windowed) co_return "the first's window's worth didn't come";

        // the second goes past it, whole, though it's queued behind it.
        // NOTE: the second's body has its window, so the writer frames it as soon as it queues, and it may have left
        //       the scheduler before a wait for two streams there looked: send()'s wait (for one) is the first's.
        queued = co_await h.send(upload(2 * frame), &outcomes->at(1));
        if (!queued) co_return "the second not queued";
        const auto second = h.ids[0];
        const bool done   = co_await h.server.wait_for(second);
        if (!done) co_return "the second unfinished";
        second_body = h.body_size(second);
        first_held  = h.body_size(first);

        // then the first, as its window grows.
        h.server.s.consume(first, initial_window);
        const bool rest = co_await h.server.wait_for(first);
        if (!rest) co_return "the first unfinished";
        first_body = h.body_size(first);
        co_return h.server.failure;
    };

    const auto failed = sched.run_to_completion(test(&sched, &outcomes));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(second_body == 2 * frame);
    CHECK(first_held == initial_window);
    CHECK(first_body == 5 * frame);
}

TEST_CASE("the client's HTTP/2 request bodies take their turns by their requests' Priority fields",
          "[http][http2][client][scheduler][priority]")
{
    // (before the scheduler, which waits for the requests)
    std::array<outcome, 7>   outcomes;
    std::string              order;
    std::vector<std::size_t> sizes; // the bodies under test's, in the order they were sent
    net::io::scheduler       sched{make_pool()};

    const auto test = [&](net::io::scheduler* sched, std::array<outcome, 7>* outcomes) -> task<std::string>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        if (!fake.has_value()) co_return fake.error();
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake->conn->close(); }};
        held             h{sched, &*fake};
        if (!h.watched) co_return "the watch wasn't spawned";

        const bool plugged = co_await h.plug(&outcomes->at(0));
        if (!plugged) co_return "not plugged";

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
            if (!queued) co_return std::format("body {} not queued", i);
        }

        const bool went = co_await h.take_turns(2);
        if (!went) co_return "not all went: " + h.frames();

        order = h.frames();
        for (const auto id : h.ids) sizes.push_back(h.body_size(id));
        co_return h.server.failure;
    };

    const auto failed = sched.run_to_completion(test(&sched, &outcomes));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    // the most urgent first; of one urgency, the non-incremental ones a body at a time by stream, the incremental
    // ones a frame each.
    CHECK(order == "FFBBCCDDAEAE");
    for (const auto size : sizes) CHECK(size == 2 * frame);
}

TEST_CASE("an HTTP/2 request body whose Priority field doesn't parse takes RFC 9218's default's turns",
          "[http][http2][client][scheduler][priority]")
{
    // (before the scheduler, which waits for the requests)
    std::array<outcome, 4> outcomes;
    std::string            order;
    net::io::scheduler     sched{make_pool()};

    const auto test = [&](net::io::scheduler* sched, std::array<outcome, 4>* outcomes) -> task<std::string>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        if (!fake.has_value()) co_return fake.error();
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake->conn->close(); }};
        held             h{sched, &*fake};
        if (!h.watched) co_return "the watch wasn't spawned";

        const bool plugged = co_await h.plug(&outcomes->at(0));
        if (!plugged) co_return "not plugged";

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
            if (!queued) co_return std::format("body {} not queued", i);
        }

        const bool went = co_await h.take_turns(2);
        if (!went) co_return "not all went: " + h.frames();
        order = h.frames();
        co_return h.server.failure;
    };

    const auto failed = sched.run_to_completion(test(&sched, &outcomes));
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(order == "BABACC");
}

TEST_CASE("an HTTP/2 request reprioritized while its body waits takes its new turns, and tells the server",
          "[http][http2][client][scheduler][priority]")
{
    // (before the scheduler, which waits for the requests)
    std::array<outcome, 4>                                   outcomes;
    std::array<bool, 3>                                      reprioritized{};    // C's, A's, and a stream's it hasn't
    bool                                                     after_close = true; // a reprioritizing once it's closed
    std::string                                              order;
    std::vector<std::pair<http2::stream_id, http::priority>> updates; // the PRIORITY_UPDATEs the server got
    std::vector<http2::stream_id>                            ids;
    net::io::scheduler                                       sched{make_pool()};

    const auto test = [&](net::io::scheduler* sched, std::array<outcome, 4>* outcomes) -> task<std::string>
    {
        auto fake = co_await connect_fake(sched, {.io_timeout = 10s});
        if (!fake.has_value()) co_return fake.error();
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake->conn->close(); }};
        held             h{sched, &*fake};
        if (!h.watched) co_return "the watch wasn't spawned";

        const bool plugged = co_await h.plug(&outcomes->at(0));
        if (!plugged) co_return "not plugged";

        // three bodies without fields, u=3 incremental, which would take turns ("ABCABC").
        for (std::size_t i = 0; i < 3; ++i)
        {
            const bool queued = co_await h.send(upload(2 * frame), &outcomes->at(i + 1));
            if (!queued) co_return std::format("body {} not queued", i);
        }
        ids = h.ids;

        // reprioritized as they wait (as client_response::set_priority() does, through the response's body): C first,
        // whole, and A after B; and the server's told. (A stream the connection doesn't have can't be.)
        reprioritized[0] = fake->conn->reprioritize(h.ids.at(2), {.urgency = 0});
        reprioritized[1] = fake->conn->reprioritize(h.ids.at(0), {.urgency = 7});
        reprioritized[2] = fake->conn->reprioritize(99, {.urgency = 1});

        const bool went = co_await h.take_turns(2);
        if (!went) co_return "not all went: " + h.frames();
        order = h.frames();

        const bool told = co_await h.server.pump([&h] { return h.server.priority_updates.size() >= 2; });
        if (!told) co_return "the server wasn't told";
        updates = h.server.priority_updates;

        // once the connection's ended, failing the streams, none can be (though the session knows nothing of it).
        fake->conn->close();
        after_close = fake->conn->reprioritize(h.ids.at(1), {.urgency = 1});
        co_return h.server.failure;
    };

    const auto failed = sched.run_to_completion(test(&sched, &outcomes));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(reprioritized[0]);
    CHECK(reprioritized[1]);
    CHECK_FALSE(reprioritized[2]);
    CHECK(order == "CCBBAA");
    REQUIRE(ids.size() == 3);
    const std::vector<std::pair<http2::stream_id, http::priority>> want{
        {ids.at(2), {.urgency = 0}},
        {ids.at(0), {.urgency = 7}},
    };
    CHECK(updates == want);
    CHECK_FALSE(after_close);
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
    // (before the scheduler, which waits for the requests, and the connection's writer)
    std::array<outcome, 4>   outcomes;
    writer_gate              gate;
    std::string              order;
    std::vector<std::size_t> sizes; // B's, U's and N's bodies'
    net::io::scheduler       sched{make_pool()};
    gate.sched = &sched;

    const auto test =
        [&](net::io::scheduler* sched, std::array<outcome, 4>* outcomes, writer_gate* gate) -> task<std::string>
    {
        // the mark set (on the real socket), so the writer waits, through the gate.
        auto fake = co_await connect_fake(
            sched,
            {.io_timeout = 10s, .notsent_lowat = std::size_t{16} * 1'024, .writable = gate->hook()});
        if (!fake.has_value()) co_return fake.error();
        // NOLINTNEXTLINE(bugprone-exception-escape): close() only throws if it can't allocate.
        net::util::defer closing{[&fake] noexcept { fake->conn->close(); }};

        // the server's windows open (16 MiB), and given back as DATA arrives: only the writer holds the bodies back.
        http2::session_config config;
        config.local.initial_window_size = std::uint32_t{16} << 20;
        config.connection_window         = std::uint32_t{16} << 20;
        test::h2_client server{&fake->socket, &fake->socket, &fake->socket, http2::role::server, config};

        std::stop_source watching;
        if (!sched->spawn(watch(sched, fake->conn, watching.get_token()))) co_return "the watch wasn't spawned";
        net::util::defer unwatch{[&watching] noexcept { watching.request_stop(); }};

        // a request first, so the settings have been exchanged (the server's windows are the client's to use).
        http::client_request get{.method = http::request_method::GET,
                                 .uri    = net::url::parse("http://example.com/").value()};
        const bool           spawned = sched->spawn(send_request(sched, fake->conn, std::move(get), &outcomes->at(0)));
        if (!spawned) co_return "the first request not sent";
        const bool asked = co_await server.pump([&server] { return server.responses.contains(1); });
        if (!asked) co_return "the first request didn't come";
        server.respond(1, "204");
        const bool answered = co_await server.pump([] { return true; });
        if (!answered) co_return server.failure.empty() ? "the response wasn't written" : server.failure;
        const bool finished = co_await (*outcomes)[0].wait(sched);
        if (!finished) co_return "the first request didn't finish";

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
                sched->spawn(send_request(sched, fake->conn, std::move(requests.at(i)), &outcomes->at(i + 1)));
            if (!sending) co_return std::format("body {} not sent", i);
            const bool queued = co_await fake->conn->wait_queued(i + 1);
            if (!queued) co_return std::format("body {} not queued", i);
        }

        // let go: B's first 4 frames (framed already), then U's, N's, and the rest of B's.
        gate->open();
        for (const http2::stream_id id : {3, 5, 7})
        {
            const bool done = co_await server.wait_for(id);
            if (!done) co_return std::format("stream {} unfinished", id);
        }

        const std::map<http2::stream_id, char> names{
            {3, 'B'},
            {5, 'U'},
            {7, 'N'}
        };
        for (const auto& [id, _] : server.data_log) order += names.contains(id) ? names.at(id) : '?';
        for (const http2::stream_id id : {3, 5, 7}) sizes.push_back(server.responses[id].body.size());
        co_return server.failure;
    };

    const auto failed = sched.run_to_completion(test(&sched, &outcomes, &gate));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(order == "BBBBUNBBBB");
    CHECK(sizes == std::vector<std::size_t>{8 * frame, frame, frame});
}
