// The server's HTTP/2 responses sent by their priorities: streams whose writes contend for the connection wait in its
// scheduler, which sends the most urgent first, non-incremental ones one at a time by stream id, and incremental ones a
// frame each in turn.
//
// To make the order the scheduler's alone, the client holds the connection's window shut: a first response (the plug)
// uses up the 65535 bytes a connection starts with, which the client never gives back; the responses under test then
// have no window, so they all queue (which a probe request waits for, through detail::wait_queued_streams()); then the
// client opens the window, and what the server sends from then on, frame by frame, is the scheduler's order.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/h2_client.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
#include "http/http2_server.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;
namespace hpack = net::http::hpack;

using namespace std::string_view_literals;
using net::coro::task;
using test::h2_client;
using test::header;
using test::stop_probe;

namespace
{

// How much a connection's window starts with: the plug's response.
constexpr std::size_t initial_window = 65'535;

// outcomes is what the handlers share with the test: how each one's write went (by its request's x-name), and how many
// have returned. (Declared before the scheduler, whose tasks use it: its sched is set once the scheduler's made.)
struct outcomes
{
    void wrote(const std::string& name, net::io::result res, std::stop_token stop)
    {
        {
            std::scoped_lock lock{mu};
            writes[name] = res;
            tokens[name] = std::move(stop);
        }
        stop_probe::bump(sched, returned);
    }

    // write_of is how the write of the handler for name went, if it's returned.
    std::optional<net::io::result> write_of(const std::string& name)
    {
        std::scoped_lock lock{mu};
        const auto       it = writes.find(name);
        if (it == writes.end()) return std::nullopt;
        return it->second;
    }

    // stopped waits (up to 5 seconds) for the handler's token to be stopped, returning whether it was: a stream's is
    // stopped once the stream has failed, after the lock its write takes, so perhaps after its write has returned.
    task<bool> stopped(const std::string& name)
    {
        std::stop_token token;
        {
            std::scoped_lock lock{mu};
            const auto       it = tokens.find(name);
            if (it == tokens.end()) co_return false;
            token = it->second;
        }
        const auto err = co_await sched->sleep_until(net::io::clock::now() + std::chrono::seconds{5}, token);
        co_return err == std::errc::operation_canceled;
    }

    net::io::scheduler*        sched = nullptr;
    std::atomic<std::uint64_t> returned{0};

    std::mutex                             mu;
    std::map<std::string, net::io::result> writes;
    std::map<std::string, std::stop_token> tokens;
};

// make_router answers /bytes with x-size bytes, written at once (with a Content-Length, or without and with a trailer
// if the request has x-trailer), and a Priority field of x-response-priority's value if it has one; and /queued (the
// probe) once x-count streams wait in the connection's scheduler, saying so in x-queued.
http::router make_router(outcomes* o)
{
    http::router r;
    r.GET("/bytes",
          [o](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              const auto size     = std::stoul(std::string{req.headers.get("x-size"sv).value_or("0"sv)});
              const bool trailing = req.headers.get("x-trailer"sv).has_value();
              if (const auto p = req.headers.get("x-response-priority"sv)) resp.headers().set("Priority", *p);

              auto* body = trailing ? co_await resp.send(http::status::OK) : co_await resp.send(http::status::OK, size);

              const std::string data(size, 'x');
              const auto        res = co_await body->write(std::as_bytes(std::span{data}));
              if (trailing) resp.trailers().set("x-sent", std::to_string(res.count));

              o->wrote(std::string{req.headers.get("x-name"sv).value_or(""sv)}, res, req.stop);
          });
    r.GET("/queued",
          [](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              const auto n      = std::stoul(std::string{req.headers.get("x-count"sv).value_or("0"sv)});
              const bool queued = co_await http::detail::wait_queued_streams(req, n);
              resp.headers().set("x-queued", queued ? "yes" : "no");
              co_await resp.send(http::status::OK, 0);
          });
    return r;
}

struct test_server
{
    test_server(net::io::scheduler* sched, outcomes* o, const http::server_config& cfg = {})
        : server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), make_router(o), cfg}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    [[nodiscard]] std::string port() const { return std::to_string(server.local_port()); }

    http::server server;
};

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// held_config is the client's session: streams' windows large, so only the connection's holds the server's DATA back,
// and that left at its initial 65535 (no WINDOW_UPDATE raising it).
http2::session_config held_config()
{
    http2::session_config config;
    config.local.initial_window_size = std::uint32_t{1} << 20;
    config.connection_window         = initial_window;
    return config;
}

// with_client connects to port, and runs test with an h2_client over the connection, holding its windows. It returns
// what went wrong, if anything: test returns what went wrong with its own steps (or the client's failure, if they
// found nothing).
template<typename Test>
task<std::string> with_client(net::io::scheduler* sched, std::string port, Test test)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
    if (!conn.has_value()) co_return "not connected: " + conn.error().message();

    h2_client client{&*conn, &*conn, &*conn, http2::role::client, held_config()};
    client.hold_windows = true;
    auto failed         = co_await test(client);
    co_return failed.empty() ? client.failure : failed;
}

// response is a request for /bytes the test makes.
struct response
{
    std::string_view name;
    std::size_t      size              = 0;
    std::string_view priority          = {}; // the request's Priority field, if any
    std::string_view response_priority = {};
    bool             trailer           = false;
};

// request_bytes sends a request for /bytes.
http2::stream_id request_bytes(h2_client& c, const response& r)
{
    const auto                size = std::to_string(r.size);
    std::vector<hpack::field> fields{
        {.name = "x-name", .value = r.name},
        {.name = "x-size",   .value = size}
    };
    if (!r.priority.empty()) fields.push_back({.name = "priority", .value = r.priority});
    if (!r.response_priority.empty()) fields.push_back({.name = "x-response-priority", .value = r.response_priority});
    if (r.trailer) fields.push_back({.name = "x-trailer", .value = "1"});
    return c.request("GET", "/bytes", {}, true, fields);
}

// held is a client whose connection's window a first response (the plug) has used up, and isn't given back: the
// responses it then requests have no window, so they wait in the server's scheduler until it gives some.
struct held
{
    explicit held(h2_client& c)
        : client{&c}
    {}

    h2_client*                    client;
    http2::stream_id              plug = 0;
    std::vector<http2::stream_id> ids; // the responses'

    // start plugs the connection, requests responses, and waits until they've all queued at the server, returning
    // whether they have.
    task<bool> start(std::span<const response> responses)
    {
        auto& c = *client;
        plug    = request_bytes(c, {.name = "plug", .size = initial_window});

        // NOTE: the waits are awaited into locals, not in the conditions: GCC 16 made `!co_await c.wait_for(probe) ||
        //       header(...) != "yes"` true when the wait had returned true and the header was "yes" (a co_await in one
        //       operand of ||, a temporary with a destructor in the other).
        const bool plugged = co_await c.wait_for(plug);
        if (!plugged || c.responses[plug].body.size() != initial_window) co_return false;

        for (const auto& r : responses) ids.push_back(request_bytes(c, r));

        // the probe, answered once they've all queued.
        const auto                      count = std::to_string(responses.size());
        const std::vector<hpack::field> fields{
            {.name = "x-count", .value = count}
        };
        const auto probe    = c.request("GET", "/queued", {}, true, fields);
        const bool answered = co_await c.wait_for(probe);
        if (!answered || header(c.responses[probe], "x-queued") != "yes") co_return false;

        c.data_log.clear();
        co_return true;
    }

    // give lets the server send n more bytes on the connection: as the plug's consumed (the client's session sends
    // WINDOW_UPDATE for the connection at once, as it has no window left), whose stream has ended.
    task<bool> give(std::size_t n) const
    {
        client->s.consume(plug, n);
        return client->pump([] { return true; }); // (what's to be written is, first)
    }

    // received waits until n DATA frames have arrived since start().
    task<bool> received(std::size_t n)
    {
        return client->pump([this, n] { return client->data_log.size() >= n; });
    }

    // finish waits until the responses are done.
    task<bool> finish()
    {
        return client->pump(
            [this] { return std::ranges::all_of(ids, [this](auto id) { return client->responses[id].done; }); });
    }

    // frames is the DATA frames received since start(), as their streams' names (names' letters, in ids' order).
    [[nodiscard]] std::string frames(std::string_view names) const
    {
        std::string out;
        for (const auto& [id, _] : client->data_log)
        {
            const auto it = std::ranges::find(ids, id);
            out += it == ids.end() ? '?' : names.at(static_cast<std::size_t>(it - ids.begin()));
        }
        return out;
    }
};

// contend queues responses, then lets the server send window bytes, and returns the order their frames came in
// (held::frames()), or what failed.
task<std::string> contend(h2_client& c, std::span<const response> responses, std::size_t window, std::string_view names)
{
    held h{c};
    if (!co_await h.start(responses)) co_return "(not queued)";
    if (!co_await h.give(window)) co_return "(not given)";
    if (!co_await h.finish()) co_return "(unfinished: " + h.frames(names) + ")";
    for (const auto id : h.ids)
    {
        if (c.responses[id].reset) co_return "(reset: " + c.responses[id].reset.message() + ")";
    }
    co_return h.frames(names);
}

// rounds says whether frames go round names, a frame each, in some order that stays the same (e.g. "BCDBCDBCD" or
// "CBDCBDCBD"), until each has sent its frames.
bool rounds(std::string_view frames, std::string_view names)
{
    if (frames.size() < names.size()) return false;

    const auto first = frames.substr(0, names.size());
    if (!std::ranges::is_permutation(first, names)) return false;
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        if (frames[i] != first[i % first.size()]) return false;
    }
    return true;
}

constexpr std::size_t frame = 16'384; // the client's MAX_FRAME_SIZE: the server's DATA frames' size

}

TEST_CASE("HTTP/2 responses that contend go most urgent first", "[http][http2][server][scheduler]")
{
    outcomes           o; // (before the scheduler, whose tasks use it)
    std::string        order;
    net::io::scheduler sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&order](h2_client& c) -> task<std::string>
                                            {
                                                // a bulk download first, then a more urgent one, and a
                                                // usual one.
                                                const std::array responses{
                                                    response{  .name = "bulk", .size = 3 * frame, .priority = "u=7"},
                                                    response{.name = "urgent", .size = 2 * frame, .priority = "u=0"},
                                                    response{ .name = "usual", .size = 2 * frame, .priority = "u=3"},
                                                };
                                                order = co_await contend(c, responses, 7 * frame, "BUN");
                                                co_return std::string{};
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(order == "UUNNBBB");
}

TEST_CASE("HTTP/2 non-incremental responses of one urgency go one at a time, by stream id",
          "[http][http2][server][scheduler]")
{
    outcomes           o; // (before the scheduler, whose tasks use it)
    std::string        order;
    net::io::scheduler sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&order](h2_client& c) -> task<std::string>
                                            {
                                                // (their handlers queue in whatever order they run.)
                                                const std::array responses{
                                                    response{.name = "a", .size = 3 * frame, .priority = "u=2"},
                                                    response{.name = "b", .size = 2 * frame, .priority = "u=2"},
                                                    response{.name = "c", .size = 3 * frame, .priority = "u=2"},
                                                    response{.name = "d", .size = 1 * frame, .priority = "u=2"},
                                                };
                                                order = co_await contend(c, responses, 9 * frame, "ABCD");
                                                co_return std::string{};
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(order == "AAABBCCCD");
}

TEST_CASE("HTTP/2 incremental responses of one urgency take turns, a frame each", "[http][http2][server][scheduler]")
{
    outcomes           o; // (before the scheduler, whose tasks use it)
    std::string        order;
    net::io::scheduler sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&order](h2_client& c) -> task<std::string>
                                            {
                                                // (in the order their handlers queued.)
                                                const std::array responses{
                                                    response{.name = "a", .size = 3 * frame, .priority = "u=4, i"},
                                                    response{.name = "b", .size = 3 * frame, .priority = "u=4, i"},
                                                    response{.name = "c", .size = 3 * frame, .priority = "u=4, i"},
                                                };
                                                order = co_await contend(c, responses, 9 * frame, "ABC");
                                                co_return std::string{};
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CAPTURE(order);
    CHECK(rounds(order, "ABC"));
}

TEST_CASE("within an urgency, HTTP/2's incremental and non-incremental responses take turns",
          "[http][http2][server][scheduler]")
{
    outcomes           o; // (before the scheduler, whose tasks use it)
    std::string        order;
    net::io::scheduler sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&order](h2_client& c) -> task<std::string>
                                            {
                                                // (the incremental one first: the scheduler's turns
                                                // start with it.)
                                                const std::array responses{
                                                    response{.name = "a", .size = 3 * frame,    .priority = "u=3"},
                                                    response{.name = "b", .size = 3 * frame, .priority = "u=3, i"},
                                                    response{.name = "c", .size = 1 * frame,    .priority = "u=3"},
                                                };
                                                order = co_await contend(c, responses, 7 * frame, "ABC");
                                                co_return std::string{};
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(order == "BABABAC");
}

TEST_CASE("HTTP/2 responses of clients that never signal priorities take turns", "[http][http2][server][scheduler]")
{
    outcomes           o; // (before the scheduler, whose tasks use it)
    std::string        order;
    net::io::scheduler sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            [&order](h2_client& c) -> task<std::string>
                                                            {
                                                                // u=3 incremental (a client that hasn't signalled
                                                                // priorities): round-robin.
                                                                const std::array responses{
                                                                    response{.name = "a", .size = 2 * frame},
                                                                    response{.name = "b", .size = 2 * frame},
                                                                    response{.name = "c", .size = 2 * frame},
                                                                };
                                                                order =
                                                                    co_await contend(c, responses, 6 * frame, "ABC");
                                                                co_return std::string{};
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CAPTURE(order);
    CHECK(rounds(order, "ABC"));
}

TEST_CASE("with http2_priorities off, HTTP/2 responses take turns whatever their priorities",
          "[http][http2][server][scheduler]")
{
    outcomes           o; // (before the scheduler, whose tasks use it)
    std::string        order;
    net::io::scheduler sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o, {.http2_priorities = false}};

    const auto failed = sched.run_to_completion(
        with_client(&sched,
                    server.port(),
                    [&order](h2_client& c) -> task<std::string>
                    {
                        const std::array responses{
                            response{.name = "a", .size = 2 * frame, .priority = "u=7"},
                            response{.name = "b", .size = 2 * frame, .priority = "u=0"},
                            response{.name = "c", .size = 2 * frame, .priority = "u=3", .response_priority = "u=0"},
                        };
                        order = co_await contend(c, responses, 6 * frame, "ABC");
                        co_return std::string{};
                    }));
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CAPTURE(order);
    CHECK(rounds(order, "ABC"));
}

TEST_CASE("a PRIORITY_UPDATE mid-response reorders HTTP/2 responses", "[http][http2][server][scheduler]")
{
    outcomes           o; // (before the scheduler, whose tasks use it)
    std::string        order;
    net::io::scheduler sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&order](h2_client& c) -> task<std::string>
                                            {
                                                const std::array responses{
                                                    response{.name = "a", .size = 3 * frame, .priority = "u=3"},
                                                    response{.name = "b", .size = 3 * frame, .priority = "u=3"},
                                                };
                                                held       h{c};
                                                const bool queued = co_await h.start(responses);
                                                if (!queued) co_return "not queued";

                                                // a's first frame, by id; then b is made more urgent.
                                                bool ok = co_await h.give(frame);
                                                if (!ok) co_return "the first frame's window not given";
                                                ok = co_await h.received(1);
                                                if (!ok) co_return "the first frame didn't come";
                                                ok = co_await c.priority_update(h.ids[1], "u=1");
                                                if (!ok) co_return "the PRIORITY_UPDATE wasn't sent";

                                                ok = co_await h.give(5 * frame);
                                                if (!ok) co_return "the rest's window not given";
                                                ok = co_await h.finish();
                                                if (!ok) co_return "unfinished: " + h.frames("AB");
                                                order = h.frames("AB");
                                                co_return std::string{};
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(order == "ABBBAA");
}

TEST_CASE("a handler's response Priority moves its HTTP/2 stream", "[http][http2][server][scheduler]")
{
    outcomes           o; // (before the scheduler, whose tasks use it)
    std::string        order;
    std::string        priority; // a's response's Priority field
    net::io::scheduler sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed = sched.run_to_completion(
        with_client(&sched,
                    server.port(),
                    [&](h2_client& c) -> task<std::string>
                    {
                        // a's is lowered to a background download (its response's u replacing
                        // its request's).
                        const std::array responses{
                            response{.name = "a", .size = 2 * frame, .priority = "u=2", .response_priority = "u=7"},
                            response{.name = "b", .size = 2 * frame, .priority = "u=2"},
                        };
                        held       h{c};
                        const bool queued = co_await h.start(responses);
                        if (!queued) co_return "not queued";
                        bool ok = co_await h.give(4 * frame);
                        if (!ok) co_return "the window not given";
                        ok = co_await h.finish();
                        if (!ok) co_return "unfinished: " + h.frames("AB");

                        order    = h.frames("AB");
                        priority = header(c.responses[h.ids[0]], "priority");
                        co_return std::string{};
                    }));
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(order == "BBAA");
    CHECK(priority == "u=7");
}

TEST_CASE("an HTTP/2 response's trailers follow its data when it contends", "[http][http2][server][scheduler]")
{
    outcomes                         o; // (before the scheduler, whose tasks use it)
    std::string                      order;
    std::vector<http2::stream_id>    ids;
    std::vector<h2_client::response> got; // the responses, in the order they were requested (ids' order)
    net::io::scheduler               sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed = sched.run_to_completion(
        with_client(&sched,
                    server.port(),
                    [&](h2_client& c) -> task<std::string>
                    {
                        const std::array responses{
                            response{.name = "a", .size = 3 * frame, .priority = "u=3, i", .trailer = true},
                            response{.name = "b", .size = 3 * frame, .priority = "u=3, i"},
                        };
                        held       h{c};
                        const bool queued = co_await h.start(responses);
                        if (!queued) co_return "not queued";
                        bool ok = co_await h.give(6 * frame);
                        if (!ok) co_return "the window not given";
                        ok = co_await h.finish();
                        if (!ok) co_return "unfinished: " + h.frames("AB");

                        order = h.frames("AB");
                        ids   = h.ids;
                        for (const auto id : h.ids) got.push_back(c.responses[id]);
                        co_return std::string{};
                    }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CAPTURE(order);
    CHECK(rounds(order, "AB"));
    REQUIRE(got.size() == 2);
    for (std::size_t i = 0; i < got.size(); ++i)
    {
        const auto& r  = got[i];
        const auto  id = ids[i];
        CAPTURE(id);
        CHECK_FALSE(r.reset); // (DATA after trailers would be malformed)
        CHECK(r.body.size() == 3 * frame);
    }
    CHECK(test::trailer(got[0], "x-sent") == std::to_string(3 * frame));
}

TEST_CASE("HTTP/2 responses with a Content-Length end with their last DATA frame when they contend",
          "[http][http2][server][scheduler][end-stream]")
{
    // framed by the writer, in turns: each one's last frame ends its stream, with no empty DATA frame after it; one of
    // a frame and a half too.
    const std::array responses{
        response{.name = "a",   .size = 3 * frame, .priority = "u=3, i"},
        response{.name = "b",   .size = 2 * frame, .priority = "u=3, i"},
        response{.name = "c", .size = frame + 100, .priority = "u=3, i"},
    };

    outcomes                         o; // (before the scheduler, whose tasks use it)
    std::string                      order;
    std::vector<h2_client::response> got; // the responses, in the order they were requested
    net::io::scheduler               sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                held       h{c};
                                                                const bool queued = co_await h.start(responses);
                                                                if (!queued) co_return "not queued";
                                                                bool ok = co_await h.give((6 * frame) + 100);
                                                                if (!ok) co_return "the window not given";
                                                                ok = co_await h.finish();
                                                                if (!ok) co_return "unfinished: " + h.frames("ABC");

                                                                order = h.frames("ABC");
                                                                for (const auto id : h.ids)
                                                                    got.push_back(c.responses[id]);
                                                                co_return std::string{};
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    // (in rounds, in the order they queued in, until each has sent its frames.)
    CAPTURE(order);
    CHECK(order.size() == 7);

    const std::array<std::size_t, 3> last{frame, frame, 100};
    const std::array<int, 3>         frames{3, 2, 2};
    REQUIRE(got.size() == responses.size());
    for (std::size_t i = 0; i < got.size(); ++i)
    {
        const auto& r = got[i];
        CAPTURE(i);
        CHECK_FALSE(r.reset);
        CHECK(r.body.size() == responses.at(i).size);
        CHECK(r.data_frames == frames.at(i));
        CHECK(r.ended_by_data == last.at(i));
    }
}

TEST_CASE("an HTTP/2 response waiting in the scheduler times out if it can't be sent",
          "[http][http2][server][scheduler][timeouts]")
{
    outcomes                     o;      // (before the scheduler, whose tasks use it)
    std::vector<std::error_code> resets; // the responses' streams' resets, in the order they were requested
    bool                         returned = false;
    std::map<std::string, bool>  stopped; // by name: whether the handler's token was stopped
    net::io::scheduler           sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o, {.write_timeout = std::chrono::seconds{1}}};

    const auto failed = sched.run_to_completion(with_client(&sched,
                                                            server.port(),
                                                            [&](h2_client& c) -> task<std::string>
                                                            {
                                                                const std::array responses{
                                                                    response{.name = "a", .size = 2 * frame},
                                                                    response{.name = "b", .size = 2 * frame},
                                                                };
                                                                held       h{c};
                                                                const bool queued = co_await h.start(responses);
                                                                if (!queued) co_return "not queued";

                                                                // the window never opens: both are reset, their writes
                                                                // failing.
                                                                const bool ok = co_await h.finish();
                                                                if (!ok) co_return "unfinished";
                                                                for (const auto id : h.ids)
                                                                    resets.push_back(c.responses[id].reset);

                                                                returned =
                                                                    co_await stop_probe::reach(o.sched, o.returned, 3);
                                                                for (const auto* name : {"a", "b"})
                                                                    stopped[name] = co_await o.stopped(name);
                                                                co_return std::string{};
                                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    for (const auto& reset : resets) CHECK(reset == http2::error_code::cancel);
    REQUIRE(returned);
    for (const auto* name : {"a", "b"})
    {
        CAPTURE(name);
        const auto res = o.write_of(name);
        REQUIRE(res.has_value());
        CHECK(res->count == 0);
        CHECK(res->err == std::errc::timed_out);
        CHECK(stopped[name]);
    }
}

TEST_CASE("an HTTP/2 response waiting in the scheduler, reset by the client, leaves it",
          "[http][http2][server][scheduler][cancel]")
{
    outcomes           o;               // (before the scheduler, whose tasks use it)
    bool               stopped = false; // a's handler's token
    std::string        order;
    std::size_t        rest = 0; // b's body's size
    net::io::scheduler sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&](h2_client& c) -> task<std::string>
                                            {
                                                const std::array responses{
                                                    response{.name = "a", .size = 3 * frame, .priority = "u=1"},
                                                    response{.name = "b", .size = 2 * frame, .priority = "u=3"},
                                                };
                                                held       h{c};
                                                const bool queued = co_await h.start(responses);
                                                if (!queued) co_return "not queued";

                                                // a's first frame; then the client resets it.
                                                bool ok = co_await h.give(frame);
                                                if (!ok) co_return "the first frame's window not given";
                                                ok = co_await h.received(1);
                                                if (!ok) co_return "the first frame didn't come";
                                                c.s.reset(h.ids[0], http2::error_code::cancel);
                                                ok = co_await c.sync();
                                                if (!ok) co_return "the reset wasn't acknowledged";

                                                ok = co_await stop_probe::reach(o.sched, o.returned, 2);
                                                if (!ok) co_return "a's handler didn't return";
                                                stopped = co_await o.stopped("a");

                                                // the rest is b's.
                                                ok = co_await h.give(2 * frame);
                                                if (!ok) co_return "the rest's window not given";
                                                ok = co_await c.pump([&] { return c.responses[h.ids[1]].done; });
                                                if (!ok) co_return "b unfinished";
                                                order = h.frames("AB");
                                                rest  = c.responses[h.ids[1]].body.size();
                                                co_return std::string{};
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    const auto res = o.write_of("a");
    REQUIRE(res.has_value());
    CHECK(res->count == frame);
    CHECK(res->err == http2::error_code::cancel);
    CHECK(stopped);
    CHECK(order == "ABB");
    CHECK(rest == 2 * frame);
}

TEST_CASE("HTTP/2 responses waiting in the scheduler fail when the client closes the connection",
          "[http][http2][server][scheduler][cancel]")
{
    outcomes                    o;       // (before the scheduler, whose tasks use it)
    std::map<std::string, bool> stopped; // by name: whether the handler's token was stopped
    net::io::scheduler          sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed = sched.run_to_completion(
        [](net::io::scheduler* sched, std::string port, outcomes* o, std::map<std::string, bool>* stopped)
            -> task<std::string>
        {
            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
            if (!conn.has_value()) co_return "not connected: " + conn.error().message();
            {
                h2_client c{&*conn, &*conn, &*conn, http2::role::client, held_config()};
                c.hold_windows = true;

                const std::array responses{
                    response{.name = "a", .size = 2 * frame,    .priority = "u=1"},
                    response{.name = "b", .size = 2 * frame, .priority = "u=3, i"},
                };
                held       h{c};
                const bool queued = co_await h.start(responses);
                if (!queued) co_return "not queued";
            }
            conn->close();

            const bool returned = co_await stop_probe::reach(sched, o->returned, 3);
            if (!returned) co_return "the handlers didn't return";
            for (const auto* name : {"a", "b"}) (*stopped)[name] = co_await o->stopped(name);
            co_return std::string{};
        }(&sched, server.port(), &o, &stopped));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    for (const auto* name : {"a", "b"})
    {
        CAPTURE(name);
        const auto res = o.write_of(name);
        REQUIRE(res.has_value());
        CHECK(res->count == 0);
        CHECK(res->err);
        CHECK(stopped[name]);
    }
}

TEST_CASE("closing the server lets HTTP/2 responses waiting in the scheduler finish",
          "[http][http2][server][scheduler]")
{
    outcomes                         o; // (before the scheduler, whose tasks use it)
    std::string                      order;
    std::vector<h2_client::response> got; // the responses, in the order they were requested
    bool                             closed = false;
    net::io::scheduler               sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed =
        sched.run_to_completion(with_client(&sched,
                                            server.port(),
                                            [&](h2_client& c) -> task<std::string>
                                            {
                                                const std::array responses{
                                                    response{.name = "a", .size = 2 * frame, .priority = "u=5"},
                                                    response{.name = "b", .size = 2 * frame, .priority = "u=1"},
                                                };
                                                held       h{c};
                                                const bool queued = co_await h.start(responses);
                                                if (!queued) co_return "not queued";

                                                // GOAWAY (and the PING after it, which the client
                                                // answers).
                                                server.server.close();
                                                bool ok = co_await c.pump([&] { return c.goaway; });
                                                if (!ok) co_return "no GOAWAY";

                                                ok = co_await h.give(4 * frame);
                                                if (!ok) co_return "the window not given";
                                                ok = co_await h.finish();
                                                if (!ok) co_return "unfinished: " + h.frames("AB");
                                                order = h.frames("AB");
                                                for (const auto id : h.ids) got.push_back(c.responses[id]);

                                                // and the connection then ends.
                                                closed = co_await c.pump([&] { return c.closed; });
                                                co_return std::string{};
                                            }));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(order == "BBAA");
    for (const auto& r : got)
    {
        CHECK_FALSE(r.reset);
        CHECK(r.body.size() == 2 * frame);
    }
    CHECK(closed);
}

TEST_CASE("an HTTP/2 response alone goes out as before: whole frames, without waiting for a turn",
          "[http][http2][server][scheduler]")
{
    constexpr std::size_t size = std::size_t{1} << 20;

    outcomes                                              o; // (before the scheduler, whose tasks use it)
    h2_client::response                                   got;
    std::vector<std::pair<http2::stream_id, std::size_t>> data_log;
    net::io::scheduler                                    sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o};

    const auto failed = sched.run_to_completion(
        [](net::io::scheduler*                                    sched,
           std::string                                            port,
           h2_client::response*                                   got,
           std::vector<std::pair<http2::stream_id, std::size_t>>* data_log) -> task<std::string>
        {
            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
            if (!conn.has_value()) co_return "not connected: " + conn.error().message();

            // windows that don't run out: the connection's 16 MiB, a stream's 4 MiB.
            http2::session_config config;
            config.local.initial_window_size = std::uint32_t{4} << 20;
            config.connection_window         = std::uint32_t{16} << 20;
            h2_client c{&*conn, &*conn, &*conn, http2::role::client, config};

            const auto id   = request_bytes(c, {.name = "alone", .size = size});
            const bool done = co_await c.wait_for(id);
            if (!done) co_return c.failure.empty() ? "unfinished" : c.failure;

            *got      = c.responses[id];
            *data_log = c.data_log;
            co_return std::string{};
        }(&sched, server.port(), &got, &data_log));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(got.body.size() == size);
    CHECK(got.data_frames == size / frame); // the last ending the stream (none empty after it)
    CHECK(got.ended_by_data == frame);
    CHECK(data_log.size() == size / frame);
    CHECK(std::ranges::all_of(data_log, [](const auto& f) { return f.second == frame; }));
}

TEST_CASE("an HTTP/2 response waiting its turn doesn't time out while the server writes others'",
          "[http][http2][server][scheduler][timeouts]")
{
    constexpr std::size_t big = std::size_t{6} << 20;

    outcomes            o; // (before the scheduler, whose tasks use it)
    h2_client::response bulk;
    std::size_t         urgent = 0; // the urgent response's body's size
    std::string         order;
    net::io::scheduler  sched{make_pool()};
    o.sched = &sched;
    test_server server{&sched, &o, {.write_timeout = std::chrono::seconds{1}}};

    const auto failed = sched.run_to_completion(
        [](net::io::scheduler*  sched,
           std::string          port,
           h2_client::response* bulk,
           std::size_t*         urgent,
           std::string*         order) -> task<std::string>
        {
            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
            if (!conn.has_value()) co_return "not connected: " + conn.error().message();

            // the streams' windows hold all of the responses, so only the socket holds them back once the
            // connection's is given (in one WINDOW_UPDATE: none follow, which would count as progress).
            auto config                      = held_config();
            config.local.initial_window_size = std::uint32_t{16} << 20;
            h2_client c{&*conn, &*conn, &*conn, http2::role::client, config};
            c.hold_windows = true;

            const std::array responses{
                response{.name = "urgent",   .size = big, .priority = "u=0"},
                response{  .name = "bulk", .size = frame, .priority = "u=7"},
            };
            held       h{c};
            const bool queued = co_await h.start(responses);
            if (!queued) co_return "not queued";
            const bool ok = co_await h.give(big + frame);
            if (!ok) co_return "the window not given";

            // the client reads slowly (64 KiB every 20 ms: about 3 MiB a second), so the urgent response takes the
            // socket for about 2 seconds, past TCP's buffers, and the bulk one waits for its turn longer than its
            // write_timeout, while the server writes.
            conn->set_read_deadline(net::io::clock::now() + std::chrono::seconds{30});
            std::vector<std::byte> buffer(std::size_t{64} * 1'024);
            while (!c.responses[h.ids[1]].done && !c.closed)
            {
                (void)co_await sched->sleep_until(net::io::clock::now() + std::chrono::milliseconds{20});
                const auto res = co_await conn->read(buffer);
                c.pending.insert(c.pending.end(),
                                 buffer.begin(),
                                 buffer.begin() + static_cast<std::ptrdiff_t>(res.count));
                if (res.err) c.closed = true;

                const auto used = c.s.receive(c.pending, c, std::chrono::steady_clock::now());
                if (!used.has_value()) co_return "the session failed: " + used.error().message();
                c.pending.erase(c.pending.begin(), c.pending.begin() + static_cast<std::ptrdiff_t>(*used));
            }

            *bulk   = c.responses[h.ids[1]];
            *urgent = c.responses[h.ids[0]].body.size();
            *order  = h.frames("UB");
            co_return std::string{};
        }(&sched, server.port(), &bulk, &urgent, &order));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CAPTURE(bulk.reset.message());
    CHECK_FALSE(bulk.reset);
    CHECK(bulk.body.size() == frame);
    CHECK(urgent == big);
    CHECK(order.find('B') == big / frame);
}
