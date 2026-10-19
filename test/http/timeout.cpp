#include "http/timeout.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception> // IWYU pragma: keep
#include <expected>
#include <memory>
#include <ratio>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/connection_pool.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/middleware.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/string_writer.hpp"
#include "io/writer.hpp"

namespace http = net::http;

namespace
{

using namespace std::chrono_literals;
using namespace std::string_view_literals;

using net::coro::task;
using net::io::clock;

// long enough that a handler waiting this long wasn't stopped, but a test that finds it so doesn't hang for long.
constexpr auto forever = 10s;

// observed is what a sleeper saw.
struct observed
{
    std::stop_token token{};         // its req.stop
    std::error_code err{};           // its wait's
    bool            stopped = false; // whether its token was stopped once the wait ended
    clock::duration waited  = {};
};

// then is what a sleeper does once its wait ends.
enum class then : std::uint8_t
{
    nothing,
    respond, // a 200, with the body "late"
    fail,    // throws std::runtime_error
};

// sleeper is a handler that waits for wait, with its request's token, then does what after says.
struct sleeper
{
    net::io::scheduler* sched = nullptr;
    clock::duration     wait  = forever;
    observed*           seen  = nullptr;
    then                after = then::nothing;

    task<void> operator()(http::server_request& req, http::response_writer& resp) const
    {
        seen->token      = req.stop;
        const auto start = clock::now();
        seen->err        = co_await sched->sleep_until(start + wait, req.stop);
        seen->waited     = clock::now() - start;
        seen->stopped    = req.stop.stop_requested();

        if (after == then::fail) throw std::runtime_error{"oops"};
        if (after == then::respond)
        {
            constexpr auto body   = "late"sv;
            auto*          writer = co_await resp.send(http::status::OK, body.size());
            co_await net::io::write_all(*writer, body);
        }
    }
};

// result is what serve got: the response, and whether the chain threw.
struct result
{
    std::string response{};
    bool        threw = false;
};

// serve runs handler (e.g. a chain) on sched's pool for a GET of /, with stop and aborted as the request's tokens, and
// waits for it to finish. The scheduler goes on running.
template<typename Handler>
result serve(net::io::scheduler& sched, const Handler& handler, std::stop_token stop = {}, std::stop_token aborted = {})
{
    http::server_request req{
        .method  = http::request_method::GET,
        .version = {.major = 1, .minor = 1}
    };
    req.target.path = "/";
    req.stop        = stop;
    req.aborted     = std::move(aborted);

    net::io::string_writer<char> out;
    http::server_response        base{
        .version = {.major = 1, .minor = 1},
        .body    = &out
    };
    http::response_writer resp{&out, &base, http::http11::response_encode};

    result            res;
    std::atomic<bool> done = false;

    const auto run = [](const Handler*         handler,
                        http::server_request*  req,
                        http::response_writer* resp,
                        result*                res,
                        std::atomic<bool>*     done) -> task<void>
    {
        try
        {
            co_await (*handler)(*req, *resp);
        }
        catch (const std::runtime_error&)
        {
            res->threw = true;
        }

        done->store(true);
        done->notify_all();
    };

    REQUIRE(sched.spawn(run(&handler, &req, &resp, &res, &done)));
    done.wait(false);

    // the token is the request's again, whatever the timeout did with it.
    CHECK(req.stop == stop);

    res.response = out.build();
    return res;
}

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(2); }

static_assert(http::middleware<http::timeout>);

}

TEST_CASE("a handler waiting past the timeout is stopped, and the timeout answers 503", "[http][middleware][timeout]")
{
    // NOTE: before the scheduler, whose tasks use it.
    observed seen;

    net::io::scheduler sched{make_pool()};

    std::stop_source request;
    const auto       start = clock::now();
    const auto       res   = serve(sched,
                                   http::chain(http::timeout{&sched, 50ms}, sleeper{.sched = &sched, .seen = &seen}),
                                   request.get_token());

    // its token, and the wait it was given, both: at the deadline, not before.
    CHECK(seen.err == std::errc::operation_canceled);
    CHECK(seen.stopped);
    CHECK(seen.waited >= 50ms);
    CHECK(clock::now() - start < forever / 2);

    CAPTURE(res.response);
    CHECK_FALSE(res.threw);
    CHECK(res.response.starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
    CHECK(res.response.contains("\r\nContent-Length: 0\r\n"));
    CHECK(res.response.ends_with("\r\n\r\n"));

    // the request's own token isn't stopped by the timeout's.
    CHECK_FALSE(request.stop_requested());
}

TEST_CASE("a response the handler sends after the timeout stands", "[http][middleware][timeout]")
{
    observed seen;

    net::io::scheduler sched{make_pool()};

    const auto res = serve(
        sched,
        http::chain(http::timeout{&sched, 20ms}, sleeper{.sched = &sched, .seen = &seen, .after = then::respond}));

    CHECK(seen.err == std::errc::operation_canceled);

    CAPTURE(res.response);
    CHECK(res.response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(res.response.ends_with("\r\n\r\nlate"));
}

TEST_CASE("a handler that throws gets the request's token back", "[http][middleware][timeout]")
{
    observed seen;

    net::io::scheduler sched{make_pool()};

    std::stop_source request;

    SECTION("before the timeout: the exception passes through")
    {
        const auto res = serve(sched,
                               http::chain(http::timeout{&sched, forever},
                                           sleeper{.sched = &sched, .wait = 1ms, .seen = &seen, .after = then::fail}),
                               request.get_token());
        CHECK_FALSE(seen.err);
        CHECK(res.threw);
        CHECK(res.response.empty());
    }

    SECTION("after the timeout: the 503 is sent, then the exception passes through")
    {
        const auto res = serve(
            sched,
            http::chain(http::timeout{&sched, 20ms}, sleeper{.sched = &sched, .seen = &seen, .after = then::fail}),
            request.get_token());
        CHECK(seen.err == std::errc::operation_canceled);
        CHECK(res.threw);
        CHECK(res.response.starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
    }

    // serve() checks that the token is the request's again.
    CHECK_FALSE(request.stop_requested());
}

TEST_CASE("a handler that finishes before the timeout is unaffected", "[http][middleware][timeout]")
{
    observed seen;

    net::io::scheduler sched{make_pool()};

    const auto res = serve(sched,
                           http::chain(http::timeout{&sched, 100ms},
                                       sleeper{.sched = &sched, .wait = 1ms, .seen = &seen, .after = then::respond}));
    CHECK_FALSE(seen.err);
    CHECK_FALSE(seen.stopped);
    CHECK(res.response.starts_with("HTTP/1.1 200 OK\r\n"));

    // its timer was canceled: the token it kept (a copy stays valid) isn't stopped after the deadline. (A call made
    // then would use the timeout's source, gone with its frame, which ASan would report.)
    std::this_thread::sleep_for(200ms);
    CHECK_FALSE(seen.token.stop_requested());
}

TEST_CASE("the request's own stop reaches the handler through the timeout's token", "[http][middleware][timeout]")
{
    observed seen;

    net::io::scheduler sched{make_pool()};

    std::stop_source request;
    std::stop_source aborted;

    const auto handler = http::chain(http::timeout{&sched, forever}, sleeper{.sched = &sched, .seen = &seen});

    SECTION("stopped while the handler waits (e.g. the client resets its stream, or the server's aborted)")
    {
        const bool by_abort = GENERATE(false, true);
        CAPTURE(by_abort);

        std::jthread stopper{[&]
                             {
                                 std::this_thread::sleep_for(50ms);
                                 if (by_abort) aborted.request_stop();
                                 request.request_stop();
                             }};

        const auto start = clock::now();
        const auto res   = serve(sched, handler, request.get_token(), aborted.get_token());

        CHECK(seen.err == std::errc::operation_canceled);
        CHECK(clock::now() - start < forever / 2);

        // not the timeout's doing, so it doesn't answer: the server sends nothing for a request stopped so.
        CHECK(res.response.empty());
    }

    SECTION("stopped already")
    {
        request.request_stop();

        const auto res = serve(sched, handler, request.get_token());
        CHECK(seen.err == std::errc::operation_canceled);
        CHECK(seen.stopped);
        CHECK(res.response.empty());
    }
}

namespace
{

// get GETs path from the server on port (over HTTP/2 with prior knowledge, if http2), returning its status and body.
task<std::pair<http::status, std::string>>
get(net::io::scheduler* sched, std::uint16_t port, std::string_view path, bool http2)
{
    http::client_options options;
    options.http2_prior_knowledge = http2;
    http::client client{sched, options};

    auto resp = co_await client.get("http://127.0.0.1:" + std::to_string(port) + std::string{path});
    if (!resp.has_value()) co_return std::pair{http::status::NONE, "error: " + resp.error().message()};

    std::string               body;
    std::array<std::byte, 64> buf{};
    while (true)
    {
        auto read = co_await resp->body->read(buf);
        body.append(reinterpret_cast<const char*>(buf.data()), read.count);
        if (read.err) break;
    }

    co_return std::pair{resp->status_code, std::move(body)};
}

}

TEST_CASE("the timeout stops a server's handler, over HTTP/1.1 and HTTP/2", "[http][middleware][timeout][server]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    // NOTE: before the scheduler, whose tasks use it.
    observed seen;

    net::io::scheduler sched{make_pool()};

    // a handler that waits with its stream open, sending nothing (over HTTP/2, nothing else would end it: its stream's
    // timeouts apply only while it reads or writes).
    http::router router;
    router.GET("/idle", sleeper{.sched = &sched, .seen = &seen});

    http::server server{
        &sched,
        http::chain(http::timeout{             &sched,       100ms},
        std::move(router)),
        {.host = "127.0.0.1", .port = "0"}
    };
    REQUIRE(sched.spawn(server.serve()));

    const auto start          = clock::now();
    const auto [status, body] = sched.run_to_completion(get(&sched, server.local_port(), "/idle", http2));

    CAPTURE(body);
    CHECK(status == http::status::SERVICE_UNAVAILABLE);
    CHECK(body.empty());
    CHECK(seen.err == std::errc::operation_canceled);
    CHECK(clock::now() - start < forever / 2);
}
