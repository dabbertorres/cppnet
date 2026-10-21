// The server's HTTP/2 streams' priorities (docs/plans/http2.md, stage 5, part 9): from the request's Priority field (or
// the connection's default, Decided question 19), then PRIORITY_UPDATE frames, then the response's Priority field,
// recorded but not yet acted on, so the tests read them with detail::stream_priority().
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/h2_client.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http2_server.hpp"
#include "http/priority.hpp"
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
namespace hpack = net::http::hpack;

using namespace std::string_view_literals;
using net::coro::task;
using test::gate;
using test::h2_client;
using test::header;
using test::stop_probe;

namespace
{

// describe is a stream's priority as the handler's response reports it: "u=3", "u=3 i", or "none" (not HTTP/2).
std::string describe(std::optional<http::priority> p)
{
    if (!p.has_value()) return "none";
    return std::format("u={}{}", p->urgency, p->incremental ? " i" : "");
}

// handlers is what the router's handler shares with the test: how many have started, and the gate they wait at (if
// the request has x-hold).
struct handlers
{
    explicit handlers(net::io::scheduler* sched)
        : sched{sched}
        , held{sched}
    {}

    net::io::scheduler*        sched;
    gate                       held;
    std::atomic<std::uint64_t> started{0};
};

// make_router answers /report with its stream's priority as it starts ("start"), after waiting at the gate if the
// request has x-hold ("held"), and once its head has been sent ("sent"), with a Priority field of x-response-priority's
// value if the request has one.
http::router make_router(handlers* h)
{
    http::router r;
    r.GET("/report",
          [h](const http::server_request& req, http::response_writer& resp) -> task<void>
          {
              const auto start = describe(http::detail::stream_priority(req));
              stop_probe::bump(h->sched, h->started);

              if (req.headers.get("x-hold"sv).has_value()) co_await h->held.pass();
              const auto held = describe(http::detail::stream_priority(req));

              if (const auto p = req.headers.get("x-response-priority"sv)) resp.headers().set("Priority", *p);
              auto*      body = co_await resp.send(http::status::OK);
              const auto sent = describe(http::detail::stream_priority(req));

              co_await net::io::write_all(*body, std::format("start={};held={};sent={}", start, held, sent));
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

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// with_client connects to port, and runs test with an h2_client over the connection.
template<typename Test>
task<> with_client(net::io::scheduler* sched, std::string port, Test test)
{
    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
    REQUIRE(conn.has_value());

    h2_client client{&*conn, &*conn, &*conn};
    co_await test(client);
}

// report sends a request for /report with fields, and returns what it reports (or the status, if it isn't a 200).
task<std::string> report(h2_client& c, std::span<const hpack::field> fields = {})
{
    const auto id       = c.request("GET", "/report", {}, true, fields);
    const bool finished = co_await c.wait_for(id);
    if (!finished) co_return "unfinished";

    const auto& r = c.responses[id];
    co_return r.status == "200" ? r.body : "status " + r.status;
}

}

TEST_CASE("an HTTP/2 stream's priority is its request's Priority field, or RFC 9218's default once the client has "
          "sent one",
          "[http][http2][server][priority]")
{
    net::io::scheduler sched{make_pool()};
    handlers           h{&sched};
    test_server        server{&sched, &h};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            // before the client has signalled a priority: u=3, but incremental, so
                                            // they'd share the connection.
                                            auto out = co_await report(c);
                                            CHECK(out == "start=u=3 i;held=u=3 i;sent=u=3 i");

                                            // its parameters, over RFC 9218's default (so i is false unless it says).
                                            const std::array urgent{
                                                hpack::field{.name = "priority", .value = "u=1"}
                                            };
                                            out = co_await report(c, urgent);
                                            CHECK(out == "start=u=1;held=u=1;sent=u=1");

                                            // from then on, a request without one is RFC 9218's default.
                                            out = co_await report(c);
                                            CHECK(out == "start=u=3;held=u=3;sent=u=3");

                                            const std::array incremental{
                                                hpack::field{.name = "priority", .value = "i"}
                                            };
                                            out = co_await report(c, incremental);
                                            CHECK(out == "start=u=3 i;held=u=3 i;sent=u=3 i");

                                            // several lines are one value, joined.
                                            const std::array lines{
                                                hpack::field{.name = "priority", .value = "u=6"},
                                                hpack::field{.name = "priority",   .value = "i"}
                                            };
                                            out = co_await report(c, lines);
                                            CHECK(out == "start=u=6 i;held=u=6 i;sent=u=6 i");

                                            // one that doesn't parse is ignored whole: the default.
                                            const std::array malformed{
                                                hpack::field{.name = "priority", .value = "u=1, U=2"}
                                            };
                                            out = co_await report(c, malformed);
                                            CHECK(out == "start=u=3;held=u=3;sent=u=3");
                                        }));
}

TEST_CASE("a malformed Priority field is still the client's signal", "[http][http2][server][priority]")
{
    net::io::scheduler sched{make_pool()};
    handlers           h{&sched};
    test_server        server{&sched, &h};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [](h2_client& c) -> task<>
                                        {
                                            // (as Go's: a client that sends the field knows of priorities.)
                                            const std::array malformed{
                                                hpack::field{.name = "priority", .value = "u="}
                                            };
                                            auto out = co_await report(c, malformed);
                                            CHECK(out == "start=u=3;held=u=3;sent=u=3");

                                            out = co_await report(c);
                                            CHECK(out == "start=u=3;held=u=3;sent=u=3");
                                        }));
}

TEST_CASE("behind an intermediary, HTTP/2 requests' Priority fields are ignored, but not PRIORITY_UPDATE",
          "[http][http2][server][priority]")
{
    net::io::scheduler sched{make_pool()};
    handlers           h{&sched};
    test_server        server{&sched, &h};

    const auto proxy_field = GENERATE(as<std::string_view>{}, "via", "forwarded", "x-forwarded-for");
    CAPTURE(proxy_field);

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&h, proxy_field](h2_client& c) -> task<>
                                        {
                                            const std::array urgent{
                                                hpack::field{.name = "priority", .value = "u=1"}
                                            };
                                            auto out = co_await report(c, urgent);
                                            CHECK(out == "start=u=1;held=u=1;sent=u=1");

                                            // a request through an intermediary: its field (its client's) is ignored,
                                            // and so are all the connection's from then on, which are unprioritized
                                            // (u=3, incremental), as if the client had never sent one.
                                            const std::array proxied{
                                                hpack::field{ .name = "priority", .value = "u=0"},
                                                hpack::field{.name = proxy_field,   .value = "x"}
                                            };
                                            out = co_await report(c, proxied);
                                            CHECK(out == "start=u=3 i;held=u=3 i;sent=u=3 i");

                                            out = co_await report(c, urgent);
                                            CHECK(out == "start=u=3 i;held=u=3 i;sent=u=3 i");

                                            out = co_await report(c);
                                            CHECK(out == "start=u=3 i;held=u=3 i;sent=u=3 i");

                                            // the intermediary's own PRIORITY_UPDATEs are applied, as is the response's
                                            // field.
                                            const std::array held{
                                                hpack::field{             .name = "x-hold",    .value = "1"},
                                                hpack::field{.name = "x-response-priority", .value = "i=?0"}
                                            };
                                            const auto id        = c.request("GET", "/report", {}, true, held);
                                            const bool delivered = co_await c.sync();
                                            REQUIRE(delivered);
                                            const bool seen = co_await stop_probe::reach(h.sched, h.started, 5);
                                            REQUIRE(seen);
                                            const bool updated = co_await c.priority_update(id, "u=5, i");
                                            REQUIRE(updated);
                                            const bool synced = co_await c.sync();
                                            REQUIRE(synced);
                                            h.held.open();

                                            const bool finished = co_await c.wait_for(id);
                                            REQUIRE(finished);
                                            CHECK(c.responses[id].body == "start=u=3 i;held=u=5 i;sent=u=5");
                                        }));
}

TEST_CASE("an HTTP/2 PRIORITY_UPDATE sent before its stream's head overrides the request's field; one sent after "
          "changes it",
          "[http][http2][server][priority]")
{
    net::io::scheduler sched{make_pool()};
    handlers           h{&sched};
    test_server        server{&sched, &h};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&h](h2_client& c) -> task<>
                                        {
                                            // stream 1's, before its head, and the client's first signal: a complete
                                            // set, so i is false.
                                            bool sent = co_await c.priority_update(1, "u=6");
                                            REQUIRE(sent);
                                            auto out = co_await report(c);
                                            CHECK(out == "start=u=6;held=u=6;sent=u=6");

                                            // the update was a signal: a request without a field is RFC 9218's default.
                                            out = co_await report(c);
                                            CHECK(out == "start=u=3;held=u=3;sent=u=3");

                                            // stream 5's, before its head, which has a field: the update's the most
                                            // recent signal.
                                            sent = co_await c.priority_update(5, "u=2");
                                            REQUIRE(sent);
                                            const std::array urgent{
                                                hpack::field{.name = "priority", .value = "u=1, i"}
                                            };
                                            out = co_await report(c, urgent);
                                            CHECK(out == "start=u=2;held=u=2;sent=u=2");

                                            // one sent while the handler runs changes the stream's.
                                            const std::array held{
                                                hpack::field{.name = "x-hold", .value = "1"}
                                            };
                                            const auto id        = c.request("GET", "/report", {}, true, held);
                                            const bool delivered = co_await c.sync();
                                            REQUIRE(delivered);
                                            const bool seen = co_await stop_probe::reach(h.sched, h.started, 4);
                                            REQUIRE(seen);
                                            sent = co_await c.priority_update(id, "u=0, i");
                                            REQUIRE(sent);
                                            const bool synced = co_await c.sync();
                                            REQUIRE(synced);
                                            h.held.open();

                                            const bool finished = co_await c.wait_for(id);
                                            REQUIRE(finished);
                                            CHECK(c.responses[id].body == "start=u=3;held=u=0 i;sent=u=0 i");

                                            // and one for a stream that has ended is ignored, the connection going on.
                                            sent = co_await c.priority_update(id, "u=7");
                                            REQUIRE(sent);
                                            out = co_await report(c);
                                            CHECK(out == "start=u=3;held=u=3;sent=u=3");
                                            CHECK_FALSE(c.goaway);
                                        }));
}

TEST_CASE("an HTTP/2 response's Priority field is merged into its stream's priority as its head is sent",
          "[http][http2][server][priority]")
{
    net::io::scheduler sched{make_pool()};
    handlers           h{&sched};
    test_server        server{&sched, &h};

    struct test_case
    {
        std::string_view request;  // the request's Priority (none if empty)
        std::string_view response; // the response's
        std::string_view want;
    };

    const auto tc =
        GENERATE(test_case{.request = "u=1, i", .response = "u=5", .want = "start=u=1 i;held=u=1 i;sent=u=5 i"},
                 test_case{.request = "u=1, i", .response = "i=?0", .want = "start=u=1 i;held=u=1 i;sent=u=1"},
                 test_case{.request = "u=1, i", .response = "u=9, x=1", .want = "start=u=1 i;held=u=1 i;sent=u=1 i"},
                 test_case{.request = "u=1, i", .response = "u=1, U", .want = "start=u=1 i;held=u=1 i;sent=u=1 i"},
                 test_case{.request = "", .response = "u=7", .want = "start=u=3 i;held=u=3 i;sent=u=7 i"});
    CAPTURE(tc.request, tc.response);

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&tc](h2_client& c) -> task<>
                                        {
                                            std::vector<hpack::field> fields{
                                                {.name = "x-response-priority", .value = tc.response}
                                            };
                                            if (!tc.request.empty())
                                                fields.push_back({.name = "priority", .value = tc.request});

                                            const auto id       = c.request("GET", "/report", {}, true, fields);
                                            const bool finished = co_await c.wait_for(id);
                                            REQUIRE(finished);

                                            // the field's sent to the client, as any other.
                                            const auto& r = c.responses[id];
                                            CHECK(r.body == tc.want);
                                            CHECK(header(r, "priority") == tc.response);
                                        }));
}

TEST_CASE("with http2_priorities off, every HTTP/2 stream is u=3 and incremental", "[http][http2][server][priority]")
{
    net::io::scheduler sched{make_pool()};
    handlers           h{&sched};
    test_server        server{&sched, &h, {.http2_priorities = false}};

    sched.run_to_completion(with_client(&sched,
                                        server.port(),
                                        [&h](h2_client& c) -> task<>
                                        {
                                            const std::array urgent{
                                                hpack::field{           .name = "priority", .value = "u=1"},
                                                hpack::field{.name = "x-response-priority", .value = "u=7"}
                                            };
                                            auto out = co_await report(c, urgent);
                                            CHECK(out == "start=u=3 i;held=u=3 i;sent=u=3 i");

                                            out = co_await report(c);
                                            CHECK(out == "start=u=3 i;held=u=3 i;sent=u=3 i");

                                            // PRIORITY_UPDATEs are still checked by the session (a malformed one ends
                                            // the connection), but ignored.
                                            const std::array held{
                                                hpack::field{.name = "x-hold", .value = "1"}
                                            };
                                            const auto id        = c.request("GET", "/report", {}, true, held);
                                            const bool delivered = co_await c.sync();
                                            REQUIRE(delivered);
                                            const bool seen = co_await stop_probe::reach(h.sched, h.started, 3);
                                            REQUIRE(seen);
                                            const bool sent = co_await c.priority_update(id, "u=0");
                                            REQUIRE(sent);
                                            const bool synced = co_await c.sync();
                                            REQUIRE(synced);
                                            h.held.open();

                                            const bool finished = co_await c.wait_for(id);
                                            REQUIRE(finished);
                                            CHECK(c.responses[id].body == "start=u=3 i;held=u=3 i;sent=u=3 i");
                                            CHECK_FALSE(c.goaway);
                                        }));
}

TEST_CASE("an HTTP/1.1 request has no stream priority", "[http][server][priority]")
{
    net::io::scheduler sched{make_pool()};
    handlers           h{&sched};
    test_server        server{&sched, &h};

    const auto out = sched.run_to_completion(
        [](net::io::scheduler* sched, std::string port) -> task<std::string>
        {
            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
            REQUIRE(conn.has_value());
            conn->set_read_timeout(std::chrono::seconds{5});

            const auto res = co_await net::io::write_all(
                *conn,
                "GET /report HTTP/1.1\r\nHost: x\r\nPriority: u=1\r\nConnection: close\r\n\r\n"sv);
            REQUIRE_FALSE(res.err);

            std::string                got;
            std::array<std::byte, 256> buf{};
            while (true)
            {
                const auto read = co_await conn->read(buf);
                got.append(reinterpret_cast<const char*>(buf.data()), read.count);
                if (read.err) break;
            }
            co_return got;
        }(&sched, server.port()));
    CAPTURE(out);
    CHECK(out.contains("start=none;held=none;sent=none"));
}
