// The server's HTTP/2 responses sent by their priorities (docs/plans/http2.md, stage 5, part 10): streams whose writes
// contend for the connection wait in its scheduler, which sends the most urgent first, non-incremental ones one at a
// time by stream id, and incremental ones a frame each in turn.
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
// have returned.
struct outcomes
{
    explicit outcomes(net::io::scheduler* sched)
        : sched{sched}
    {}

    void wrote(const std::string& name, net::io::result res, std::stop_token stop)
    {
        {
            std::scoped_lock lock{mu};
            writes[name] = res;
            tokens[name] = std::move(stop);
        }
        stop_probe::bump(sched, returned);
    }

    net::io::result write_of(const std::string& name)
    {
        std::scoped_lock lock{mu};
        return writes.at(name);
    }

    // stopped waits (up to 5 seconds) for the handler's token to be stopped, returning whether it was: a stream's is
    // stopped once the stream has failed, after the lock its write takes, so perhaps after its write has returned.
    task<bool> stopped(const std::string& name)
    {
        std::stop_token token;
        {
            std::scoped_lock lock{mu};
            token = tokens.at(name);
        }
        const auto err = co_await sched->sleep_until(net::io::clock::now() + std::chrono::seconds{5}, token);
        co_return err == std::errc::operation_canceled;
    }

    net::io::scheduler*        sched;
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

// with_client connects to port, and runs test with an h2_client over the connection, holding its windows.
template<typename Test>
task<> with_client(net::io::scheduler* sched, std::string port, Test test)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
    REQUIRE(conn.has_value());

    h2_client client{&*conn, &*conn, &*conn, http2::role::client, held_config()};
    client.hold_windows = true;
    co_await test(client);
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
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            // a bulk download first, then a more urgent one, and a usual one.
                                            const std::array responses{
                                                response{  .name = "bulk", .size = 3 * frame, .priority = "u=7"},
                                                response{.name = "urgent", .size = 2 * frame, .priority = "u=0"},
                                                response{ .name = "usual", .size = 2 * frame, .priority = "u=3"},
                                            };
                                            const auto order = co_await contend(c, responses, 7 * frame, "BUN");
                                            CHECK(order == "UUNNBBB");
                                        }));
}

TEST_CASE("HTTP/2 non-incremental responses of one urgency go one at a time, by stream id",
          "[http][http2][server][scheduler]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            // (their handlers queue in whatever order they run.)
                                            const std::array responses{
                                                response{.name = "a", .size = 3 * frame, .priority = "u=2"},
                                                response{.name = "b", .size = 2 * frame, .priority = "u=2"},
                                                response{.name = "c", .size = 3 * frame, .priority = "u=2"},
                                                response{.name = "d", .size = 1 * frame, .priority = "u=2"},
                                            };
                                            const auto order = co_await contend(c, responses, 9 * frame, "ABCD");
                                            CHECK(order == "AAABBCCCD");
                                        }));
}

TEST_CASE("HTTP/2 incremental responses of one urgency take turns, a frame each", "[http][http2][server][scheduler]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            // (in the order their handlers queued.)
                                            const std::array responses{
                                                response{.name = "a", .size = 3 * frame, .priority = "u=4, i"},
                                                response{.name = "b", .size = 3 * frame, .priority = "u=4, i"},
                                                response{.name = "c", .size = 3 * frame, .priority = "u=4, i"},
                                            };
                                            const auto order = co_await contend(c, responses, 9 * frame, "ABC");
                                            CAPTURE(order);
                                            CHECK(rounds(order, "ABC"));
                                        }));
}

TEST_CASE("within an urgency, HTTP/2's incremental and non-incremental responses take turns",
          "[http][http2][server][scheduler]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            // (the incremental one first: the scheduler's turns start with it.)
                                            const std::array responses{
                                                response{.name = "a", .size = 3 * frame,    .priority = "u=3"},
                                                response{.name = "b", .size = 3 * frame, .priority = "u=3, i"},
                                                response{.name = "c", .size = 1 * frame,    .priority = "u=3"},
                                            };
                                            const auto order = co_await contend(c, responses, 7 * frame, "ABC");
                                            CHECK(order == "BABABAC");
                                        }));
}

TEST_CASE("HTTP/2 responses of clients that never signal priorities take turns", "[http][http2][server][scheduler]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            // u=3 incremental (Decided question 19 of stage 5): round-robin.
                                            const std::array responses{
                                                response{.name = "a", .size = 2 * frame},
                                                response{.name = "b", .size = 2 * frame},
                                                response{.name = "c", .size = 2 * frame},
                                            };
                                            const auto order = co_await contend(c, responses, 6 * frame, "ABC");
                                            CAPTURE(order);
                                            CHECK(rounds(order, "ABC"));
                                        }));
}

TEST_CASE("with http2_priorities off, HTTP/2 responses take turns whatever their priorities",
          "[http][http2][server][scheduler]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o, {.http2_priorities = false}};

    sched.run_to_completion(
        with_client(&sched,
                    server.port(),
                    [](h2_client& c) -> task<>
                    {
                        const std::array responses{
                            response{.name = "a", .size = 2 * frame, .priority = "u=7"},
                            response{.name = "b", .size = 2 * frame, .priority = "u=0"},
                            response{.name = "c", .size = 2 * frame, .priority = "u=3", .response_priority = "u=0"},
                        };
                        const auto order = co_await contend(c, responses, 6 * frame, "ABC");
                        CAPTURE(order);
                        CHECK(rounds(order, "ABC"));
                    }));
}

TEST_CASE("a PRIORITY_UPDATE mid-response reorders HTTP/2 responses", "[http][http2][server][scheduler]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            const std::array responses{
                                                response{.name = "a", .size = 3 * frame, .priority = "u=3"},
                                                response{.name = "b", .size = 3 * frame, .priority = "u=3"},
                                            };
                                            held       h{c};
                                            const bool queued = co_await h.start(responses);
                                            REQUIRE(queued);

                                            // a's first frame, by id; then b is made more urgent.
                                            bool ok = co_await h.give(frame);
                                            REQUIRE(ok);
                                            ok = co_await h.received(1);
                                            REQUIRE(ok);
                                            ok = co_await c.priority_update(h.ids[1], "u=1");
                                            REQUIRE(ok);

                                            ok = co_await h.give(5 * frame);
                                            REQUIRE(ok);
                                            ok = co_await h.finish();
                                            REQUIRE(ok);
                                            CHECK(h.frames("AB") == "ABBBAA");
                                        }));
}

TEST_CASE("a handler's response Priority moves its HTTP/2 stream", "[http][http2][server][scheduler]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(
        with_client(&sched,
                    server.port(),
                    [](h2_client& c) -> task<>
                    {
                        // a's is lowered to a background download (its response's u replacing
                        // its request's).
                        const std::array responses{
                            response{.name = "a", .size = 2 * frame, .priority = "u=2", .response_priority = "u=7"},
                            response{.name = "b", .size = 2 * frame, .priority = "u=2"},
                        };
                        held       h{c};
                        const bool queued = co_await h.start(responses);
                        REQUIRE(queued);
                        bool ok = co_await h.give(4 * frame);
                        REQUIRE(ok);
                        ok = co_await h.finish();
                        REQUIRE(ok);

                        CHECK(h.frames("AB") == "BBAA");
                        CHECK(header(c.responses[h.ids[0]], "priority") == "u=7");
                    }));
}

TEST_CASE("an HTTP/2 response's trailers follow its data when it contends", "[http][http2][server][scheduler]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(
        with_client(&sched,
                    server.port(),
                    [](h2_client& c) -> task<>
                    {
                        const std::array responses{
                            response{.name = "a", .size = 3 * frame, .priority = "u=3, i", .trailer = true},
                            response{.name = "b", .size = 3 * frame, .priority = "u=3, i"},
                        };
                        held       h{c};
                        const bool queued = co_await h.start(responses);
                        REQUIRE(queued);
                        bool ok = co_await h.give(6 * frame);
                        REQUIRE(ok);
                        ok = co_await h.finish();
                        REQUIRE(ok);

                        const auto order = h.frames("AB");
                        CAPTURE(order);
                        CHECK(rounds(order, "AB"));
                        for (const auto id : h.ids)
                        {
                            const auto& r = c.responses[id];
                            CAPTURE(id);
                            CHECK_FALSE(r.reset); // (DATA after trailers would be malformed)
                            CHECK(r.body.size() == 3 * frame);
                        }
                        CHECK(test::trailer(c.responses[h.ids[0]], "x-sent") == std::to_string(3 * frame));
                    }));
}

TEST_CASE("HTTP/2 responses with a Content-Length end with their last DATA frame when they contend",
          "[http][http2][server][scheduler][end-stream]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            // framed by the writer, in turns: each one's last frame ends its stream
                                            // (docs/plans/http2.md, stage 6's first part), with no empty DATA frame
                                            // after it; one of a frame and a half too.
                                            const std::array responses{
                                                response{.name = "a",   .size = 3 * frame, .priority = "u=3, i"},
                                                response{.name = "b",   .size = 2 * frame, .priority = "u=3, i"},
                                                response{.name = "c", .size = frame + 100, .priority = "u=3, i"},
                                            };
                                            held       h{c};
                                            const bool queued = co_await h.start(responses);
                                            REQUIRE(queued);
                                            bool ok = co_await h.give((6 * frame) + 100);
                                            REQUIRE(ok);
                                            ok = co_await h.finish();
                                            REQUIRE(ok);

                                            // (in rounds, in the order they queued in, until each has sent its frames.)
                                            const auto order = h.frames("ABC");
                                            CAPTURE(order);
                                            CHECK(order.size() == 7);

                                            const std::array<std::size_t, 3> last{frame, frame, 100};
                                            const std::array<int, 3>         frames{3, 2, 2};
                                            for (std::size_t i = 0; i < h.ids.size(); ++i)
                                            {
                                                const auto& r = c.responses[h.ids[i]];
                                                CAPTURE(i);
                                                CHECK_FALSE(r.reset);
                                                CHECK(r.body.size() == responses.at(i).size);
                                                CHECK(r.data_frames == frames.at(i));
                                                CHECK(r.ended_by_data == last.at(i));
                                            }
                                        }));
}

TEST_CASE("an HTTP/2 response waiting in the scheduler times out if it can't be sent",
          "[http][http2][server][scheduler][timeouts]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o, {.write_timeout = std::chrono::seconds{1}}};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&o](h2_client& c) -> task<>
                                        {
                                            const std::array responses{
                                                response{.name = "a", .size = 2 * frame},
                                                response{.name = "b", .size = 2 * frame},
                                            };
                                            held       h{c};
                                            const bool queued = co_await h.start(responses);
                                            REQUIRE(queued);

                                            // the window never opens: both are reset, their writes failing.
                                            const bool ok = co_await h.finish();
                                            REQUIRE(ok);
                                            for (const auto id : h.ids)
                                                CHECK(c.responses[id].reset == http2::error_code::cancel);

                                            const bool returned = co_await stop_probe::reach(o.sched, o.returned, 3);
                                            REQUIRE(returned);
                                            for (const auto* name : {"a", "b"})
                                            {
                                                CAPTURE(name);
                                                const auto res = o.write_of(name);
                                                CHECK(res.count == 0);
                                                CHECK(res.err == std::errc::timed_out);
                                                const bool stopped = co_await o.stopped(name);
                                                CHECK(stopped);
                                            }
                                        }));
}

TEST_CASE("an HTTP/2 response waiting in the scheduler, reset by the client, leaves it",
          "[http][http2][server][scheduler][cancel]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&o](h2_client& c) -> task<>
                                        {
                                            const std::array responses{
                                                response{.name = "a", .size = 3 * frame, .priority = "u=1"},
                                                response{.name = "b", .size = 2 * frame, .priority = "u=3"},
                                            };
                                            held       h{c};
                                            const bool queued = co_await h.start(responses);
                                            REQUIRE(queued);

                                            // a's first frame; then the client resets it.
                                            bool ok = co_await h.give(frame);
                                            REQUIRE(ok);
                                            ok = co_await h.received(1);
                                            REQUIRE(ok);
                                            c.s.reset(h.ids[0], http2::error_code::cancel);
                                            ok = co_await c.sync();
                                            REQUIRE(ok);

                                            ok = co_await stop_probe::reach(o.sched, o.returned, 2);
                                            REQUIRE(ok);
                                            const auto res = o.write_of("a");
                                            CHECK(res.count == frame);
                                            CHECK(res.err == http2::error_code::cancel);
                                            const bool stopped = co_await o.stopped("a");
                                            CHECK(stopped);

                                            // the rest is b's.
                                            ok = co_await h.give(2 * frame);
                                            REQUIRE(ok);
                                            ok = co_await c.pump([&] { return c.responses[h.ids[1]].done; });
                                            REQUIRE(ok);
                                            CHECK(h.frames("AB") == "ABB");
                                            CHECK(c.responses[h.ids[1]].body.size() == 2 * frame);
                                        }));
}

TEST_CASE("HTTP/2 responses waiting in the scheduler fail when the client closes the connection",
          "[http][http2][server][scheduler][cancel]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(
        [](net::io::scheduler* sched, std::string port, outcomes* o) -> task<>
        {
            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
            REQUIRE(conn.has_value());
            {
                h2_client c{&*conn, &*conn, &*conn, http2::role::client, held_config()};
                c.hold_windows = true;

                const std::array responses{
                    response{.name = "a", .size = 2 * frame,    .priority = "u=1"},
                    response{.name = "b", .size = 2 * frame, .priority = "u=3, i"},
                };
                held       h{c};
                const bool queued = co_await h.start(responses);
                REQUIRE(queued);
            }
            conn->close();

            const bool returned = co_await stop_probe::reach(sched, o->returned, 3);
            REQUIRE(returned);
            for (const auto* name : {"a", "b"})
            {
                CAPTURE(name);
                const auto res = o->write_of(name);
                CHECK(res.count == 0);
                CHECK(res.err);
                const bool stopped = co_await o->stopped(name);
                CHECK(stopped);
            }
        }(&sched, server.port(), &o));
}

TEST_CASE("closing the server lets HTTP/2 responses waiting in the scheduler finish",
          "[http][http2][server][scheduler]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&server](h2_client& c) -> task<>
                                        {
                                            const std::array responses{
                                                response{.name = "a", .size = 2 * frame, .priority = "u=5"},
                                                response{.name = "b", .size = 2 * frame, .priority = "u=1"},
                                            };
                                            held       h{c};
                                            const bool queued = co_await h.start(responses);
                                            REQUIRE(queued);

                                            // GOAWAY (and the PING after it, which the client answers).
                                            server.server.close();
                                            bool ok = co_await c.pump([&] { return c.goaway; });
                                            REQUIRE(ok);

                                            ok = co_await h.give(4 * frame);
                                            REQUIRE(ok);
                                            ok = co_await h.finish();
                                            REQUIRE(ok);
                                            CHECK(h.frames("AB") == "BBAA");
                                            for (const auto id : h.ids)
                                            {
                                                CHECK_FALSE(c.responses[id].reset);
                                                CHECK(c.responses[id].body.size() == 2 * frame);
                                            }

                                            // and the connection then ends.
                                            ok = co_await c.pump([&] { return c.closed; });
                                            CHECK(ok);
                                        }));
}

TEST_CASE("an HTTP/2 response alone goes out as before: whole frames, without waiting for a turn",
          "[http][http2][server][scheduler]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o};

    sched.run_to_completion(
        [](net::io::scheduler* sched, std::string port) -> task<>
        {
            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
            REQUIRE(conn.has_value());

            // windows that don't run out: the connection's 16 MiB, a stream's 4 MiB.
            http2::session_config config;
            config.local.initial_window_size = std::uint32_t{4} << 20;
            config.connection_window         = std::uint32_t{16} << 20;
            h2_client c{&*conn, &*conn, &*conn, http2::role::client, config};

            constexpr std::size_t size = std::size_t{1} << 20;
            const auto            id   = request_bytes(c, {.name = "alone", .size = size});
            const bool            done = co_await c.wait_for(id);
            REQUIRE(done);

            const auto& r = c.responses[id];
            CHECK(r.body.size() == size);
            CHECK(r.data_frames == size / frame); // the last ending the stream (none empty after it)
            CHECK(r.ended_by_data == frame);
            CHECK(c.data_log.size() == size / frame);
            CHECK(std::ranges::all_of(c.data_log, [](const auto& f) { return f.second == frame; }));
        }(&sched, server.port()));
}

TEST_CASE("an HTTP/2 response waiting its turn doesn't time out while the server writes others'",
          "[http][http2][server][scheduler][timeouts]")
{
    net::io::scheduler sched{make_pool()};
    outcomes           o{&sched};
    test_server        server{&sched, &o, {.write_timeout = std::chrono::seconds{1}}};

    sched.run_to_completion(
        [](net::io::scheduler* sched, std::string port) -> task<>
        {
            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
            REQUIRE(conn.has_value());

            // the streams' windows hold all of the responses, so only the socket holds them back once the
            // connection's is given (in one WINDOW_UPDATE: none follow, which would count as progress).
            auto config                      = held_config();
            config.local.initial_window_size = std::uint32_t{16} << 20;
            h2_client c{&*conn, &*conn, &*conn, http2::role::client, config};
            c.hold_windows = true;

            constexpr std::size_t big = std::size_t{6} << 20;
            const std::array      responses{
                response{.name = "urgent",   .size = big, .priority = "u=0"},
                response{  .name = "bulk", .size = frame, .priority = "u=7"},
            };
            held       h{c};
            const bool queued = co_await h.start(responses);
            REQUIRE(queued);
            bool ok = co_await h.give(big + frame);
            REQUIRE(ok);

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
                REQUIRE(used.has_value());
                c.pending.erase(c.pending.begin(), c.pending.begin() + static_cast<std::ptrdiff_t>(*used));
            }

            const auto& bulk = c.responses[h.ids[1]];
            CAPTURE(bulk.reset.message());
            CHECK_FALSE(bulk.reset);
            CHECK(bulk.body.size() == frame);
            CHECK(c.responses[h.ids[0]].body.size() == big);
            CHECK(h.frames("UB").find('B') == big / frame);
        }(&sched, server.port()));
}
