// bench_request measures what the server does for each request: decoding it (HTTP/1.1), routing it to a handler, the
// handler's response, and encoding that, with TechEmpower's plaintext requests. It counts heap allocations per request
// too. Four ways:
// - pipelined: 16 requests read together (as a client pipelining them sends them), from memory rather than a socket.
// - one per read: the same, but each read hands over just one request, and has to wait for it (a coroutine, as a
//   socket's read that has to wait is): as for a client that waits for each response, apart from the socket.
// - over a socket: the server itself, and a client on another thread sending a request at a time (as TechEmpower's
//   JSON test does), so with the socket, scheduler and kernel's round trip too. And again with a handler that waits
//   once (for another pool, as for a database's driver), whose connection the server watches meanwhile, to notice the
//   client leaving.
// - over HTTP/2: the same over cleartext HTTP/2 (prior knowledge), a stream at a time from cppnet's own client, whose
//   allocations are counted with the server's (as they share the process): for what a stream costs.
// And pipelined again, through a chain of middleware (instrument::http_metrics) around the router: for what a chain
// costs (see docs/plans/middleware.md), with the metrics' own work; and with the metrics on the route instead (use()),
// with each router: for what middleware on routes costs. And through http::timeout on the route, pipelined and over a
// socket, with a deadline that never comes: for what its timer costs.
#include "http/request.hpp"

#include <algorithm>
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
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/connection_pool.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/middleware.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "http/static_router.hpp"
#include "http/timeout.hpp"
#include "instrument/middleware.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/discard_writer.hpp"
#include "io/flush_before_read.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/string_reader.hpp"
#include "io/writer.hpp"

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

using http::literals::operator""_route;
using namespace std::string_view_literals;
using namespace std::chrono_literals;
using net::coro::task;

// as bench/web/cppnet's, and wrk sends it, with TechEmpower's headers.
constexpr auto request =
    "GET /plaintext HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Accept: text/plain,text/html;q=0.9,application/xhtml+xml;q=0.9,application/xml;q=0.8,*/*;q=0.7\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"sv;

// the same, to a handler that waits once.
constexpr auto waiting_request =
    "GET /waiting HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Accept: text/plain,text/html;q=0.9,application/xhtml+xml;q=0.9,application/xml;q=0.8,*/*;q=0.7\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"sv;

constexpr auto body = "Hello, World!"sv;

constexpr std::size_t pipelined       = 16;
constexpr int         batches         = 50'000;
constexpr int         repeats         = 7;
constexpr std::size_t date_bytes      = 29; // e.g. "Sun, 06 Nov 1994 08:49:37 GMT"
constexpr std::size_t buffer_size     = std::size_t{16} * 1'024;
constexpr int         socket_requests = 50'000;
constexpr int         http2_requests  = 20'000;

task<void> plaintext(const http::server_request& /*req*/, http::response_writer& resp)
{
    resp.headers().set("Server"sv, "cppnet"sv).set("Content-Type"sv, "text/plain"sv);
    auto* out = co_await resp.send(http::status::OK, body.size());
    co_await net::io::write_all(*out, body);
}

// waits_once answers as plaintext does, after waiting once: for a turn on another pool.
struct waits_once
{
    net::coro::thread_pool* elsewhere;

    task<void> operator()(const http::server_request& req, http::response_writer& resp) const
    {
        co_await elsewhere->yield();
        co_await plaintext(req, resp);
    }
};

// one_request_per_read hands over its input a request at a time, each through a coroutine (as a socket's read that has
// to wait for data is).
class one_request_per_read final : public net::io::reader
{
public:
    explicit one_request_per_read(std::string_view input) noexcept
        : input{input}
    {}

    task<net::io::result> read(std::span<std::byte> data) override { return wait_then_read(data); }

    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

private:
    task<net::io::result> wait_then_read(std::span<std::byte> data)
    {
        if (input.empty()) co_return net::io::result{.err = make_error_code(net::io::errc::end_of_stream)};

        // the rest of the current request, or as much as fits.
        const auto next = std::min({data.size(), input.size(), request.size() - (offset % request.size())});
        std::ranges::copy(std::as_bytes(std::span{input.substr(0, next)}), data.begin());
        input.remove_prefix(next);
        offset += next;

        co_return net::io::result{.count = next};
    }

    std::string_view input;
    std::size_t      offset = 0;
};

// steps counts allocations at each step of serving a request.
struct steps
{
    std::size_t decode   = 0;
    std::size_t handle   = 0; // routing, the handler, and encoding its response
    std::size_t finish   = 0; // the rest: e.g. the server's own headers, and skipping the request's body
    std::size_t requests = 0;
};

// serve serves the requests from input, as server::serve_connection and serve_request do (without the logging and
// error handling).
template<typename Router>
task<void> serve(const Router& router, net::io::reader& input, net::io::writer& out, steps& counted)
{
    net::io::buffered_writer   writer{&out, buffer_size};
    net::io::flush_before_read flushing{&input, &writer};
    net::io::buffered_reader   reader{&flushing, buffer_size};

    const std::string date(date_bytes, 'x');

    // kept for the connection, as the server does.
    http::server_request  req;
    http::server_response resp;

    while (true)
    {
        auto at = allocations.load(std::memory_order::relaxed);

        if (auto err = co_await http::http11::request_decode(&reader, 8'192, req); err) break;

        counted.decode += allocations.load(std::memory_order::relaxed) - at;
        at = allocations.load(std::memory_order::relaxed);

        resp.version     = req.version;
        resp.status_code = http::status::NONE;
        resp.body        = &writer;
        resp.headers.clear();
        resp.headers.set("Date"sv, std::string_view{date});

        http::response_writer rw{&writer, &resp, http::http11::response_encode};

        counted.finish += allocations.load(std::memory_order::relaxed) - at;
        at = allocations.load(std::memory_order::relaxed);

        co_await router(req, rw);

        counted.handle += allocations.load(std::memory_order::relaxed) - at;
        at = allocations.load(std::memory_order::relaxed);

        // skips what's left of the body (none, here).
        std::array<std::byte, 64> scratch{};
        while (!(co_await req.body->read(scratch)).err)
        {}

        counted.finish += allocations.load(std::memory_order::relaxed) - at;
        ++counted.requests;
    }

    co_await writer.flush();
}

// measure serves batches of requests from memory, with make_input making the reader for each batch's input, and prints
// the time and allocations per request.
template<typename Router, typename MakeInput>
void measure(std::string_view name, const Router& router, MakeInput make_input)
{
    std::string input;
    for (std::size_t i = 0; i < pipelined; ++i) input += request;

    net::io::discard_writer out;
    std::vector<double>     runs;

    for (int r = 0; r < repeats; ++r)
    {
        steps counted;
        auto  start = std::chrono::steady_clock::now();

        for (int i = 0; i < batches; ++i)
        {
            auto in     = make_input(input);
            auto served = serve(router, in, out, counted);
            served.get_handle().resume();
        }

        auto took = std::chrono::steady_clock::now() - start;
        if (counted.requests != pipelined * batches) std::println(stderr, "served {} requests", counted.requests);

        runs.push_back(std::chrono::duration<double, std::nano>(took).count() / static_cast<double>(counted.requests));
    }

    std::ranges::sort(runs);

    // and once more, counting allocations at each step (the first request of a batch's are spread over the 16).
    steps counted;
    auto  in     = make_input(input);
    auto  served = serve(router, in, out, counted);
    served.get_handle().resume();

    const auto per_request = [&](std::size_t n)
    { return static_cast<double>(n) / static_cast<double>(counted.requests); };

    std::println("{}:", name);
    std::println("{:>10.1f} ns per request (best of {})", runs.front(), repeats);
    std::println("{:>10.1f} allocations per request: decoding {:.1f}, handling {:.1f}, the rest {:.1f}",
                 per_request(counted.decode + counted.handle + counted.finish),
                 per_request(counted.decode),
                 per_request(counted.handle),
                 per_request(counted.finish));
}

// client is a blocking client, which sends a request at a time.
class client
{
public:
    client(std::uint16_t port, std::string_view request)
        : fd{::socket(AF_INET, SOCK_STREAM, 0)}
        , request{request}
    {
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (fd < 0)
        {
            std::println(stderr, "failed to make a socket");
            return;
        }

        // NOTE: the server listens from when it's constructed.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API.
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
            std::println(stderr, "failed to connect to the server");
    }

    client(const client&)            = delete;
    client& operator=(const client&) = delete;
    client(client&&)                 = delete;
    client& operator=(client&&)      = delete;

    ~client() { ::close(fd); }

    // round_trip sends a request, and reads its response (which ends with the body), without allocating.
    bool round_trip()
    {
        if (::send(fd, request.data(), request.size(), 0) != static_cast<ssize_t>(request.size())) return false;

        std::size_t got = 0;
        while (true)
        {
            auto n = ::recv(fd, buf.data() + got, buf.size() - got, 0);
            if (n <= 0) return false;
            got += static_cast<std::size_t>(n);

            if (std::string_view{buf.data(), got}.ends_with(body)) return true;
        }
    }

private:
    int                     fd;
    std::string_view        request;
    std::array<char, 4'096> buf{};
};

// measure_socket serves requests (each request) over a socket, a request at a time, and prints the time and allocations
// per request.
template<typename MakeRouter>
void measure_socket(std::string_view name, MakeRouter make_router, std::string_view request)
{
    auto               workers = std::make_shared<net::coro::thread_pool>(2);
    net::io::scheduler scheduler{workers};

    http::server server{
        &scheduler,
        make_router(&scheduler),
        {.host = "127.0.0.1", .port = "0"}
    };

    scheduler.spawn(server.serve());
    std::thread runner{[&] { scheduler.run(); }};

    {
        client conn{server.local_port(), request};

        // warms up: e.g. the connection's buffers, and its request and response, which are kept for it.
        for (int i = 0; i < 1'000; ++i)
        {
            if (!conn.round_trip()) std::println(stderr, "warm-up request failed");
        }

        const auto at    = allocations.load();
        const auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < socket_requests; ++i)
        {
            if (!conn.round_trip())
            {
                std::println(stderr, "request {} failed", i);
                break;
            }
        }

        const auto took    = std::chrono::steady_clock::now() - start;
        const auto counted = allocations.load() - at;

        std::println("{} ({} workers):", name, workers->concurrency());
        std::println("{:>10.1f} us per request",
                     std::chrono::duration<double, std::micro>(took).count() / socket_requests);
        std::println("{:>10.1f} allocations per request",
                     static_cast<double>(counted) / static_cast<double>(socket_requests));
    }

    server.close();
    scheduler.shutdown();
    runner.join();
}

// http2_outcome is what measure_http2's client measured.
struct http2_outcome
{
    std::size_t              allocations = 0;
    std::chrono::nanoseconds took{};
    int                      failed = -1; // the request that failed, if one did
};

// get_all GETs url, and reads its response's body to the end, returning whether it got all of it.
task<bool> get_all(http::client& client, std::string_view url)
{
    auto resp = co_await client.get(url);
    if (!resp.has_value()) co_return false;

    std::array<std::byte, 64> scratch{};
    while (true)
    {
        auto res = co_await resp->body->read(scratch);
        if (res.err) co_return res.err == net::io::errc::end_of_stream;
    }
}

// send_http2_requests sends requests to url one at a time, after warming up, counting the allocations meanwhile.
task<http2_outcome> send_http2_requests(http::client* client, std::string url)
{
    // warms up: e.g. the connection, and the session's buffers.
    for (int i = 0; i < 1'000; ++i)
    {
        if (!co_await get_all(*client, url)) co_return http2_outcome{.failed = i};
    }

    http2_outcome out;
    const auto    at    = allocations.load();
    const auto    start = std::chrono::steady_clock::now();

    for (int i = 0; i < http2_requests; ++i)
    {
        if (!co_await get_all(*client, url))
        {
            out.failed = i;
            break;
        }
    }

    out.took        = std::chrono::steady_clock::now() - start;
    out.allocations = allocations.load() - at;
    co_return out;
}

// measure_http2 serves requests over cleartext HTTP/2, a stream at a time from cppnet's client, and prints the time and
// allocations per request (the client's and the server's together).
template<typename Router>
void measure_http2(const Router& router)
{
    auto               workers = std::make_shared<net::coro::thread_pool>(2);
    net::io::scheduler scheduler{workers};

    http::server server{
        &scheduler,
        router,
        {.host = "127.0.0.1", .port = "0"}
    };
    scheduler.spawn(server.serve());

    http::client_options options;
    options.http2_prior_knowledge = true;
    http::client client{&scheduler, options};

    const auto url = std::format("http://127.0.0.1:{}/plaintext", server.local_port());
    const auto out = scheduler.run_to_completion(send_http2_requests(&client, url));
    if (out.failed >= 0) std::println(stderr, "HTTP/2 request {} failed", out.failed);

    std::println("over HTTP/2 (a stream at a time, {} workers; the client's allocations too):", workers->concurrency());
    std::println("{:>10.1f} us per request",
                 std::chrono::duration<double, std::micro>(out.took).count() / http2_requests);
    std::println("{:>10.1f} allocations per request",
                 static_cast<double>(out.allocations) / static_cast<double>(http2_requests));

    server.close();
    scheduler.shutdown();
}

}

int main()
{
    spdlog::set_level(spdlog::level::warn);

    net::coro::thread_pool elsewhere{1};

    const auto router  = http::make_static_router(http::GET("/plaintext"_route, plaintext));
    const auto waiting = http::make_static_router(http::GET("/waiting"_route, waits_once{&elsewhere}));

    measure("pipelined", router, [](std::string_view input) { return net::io::string_reader{input}; });
    measure("one per read", router, [](std::string_view input) { return one_request_per_read{input}; });
    measure_socket(
        "over a socket, a request at a time",
        [&](net::io::scheduler* /*sched*/) { return router; },
        request);
    measure_socket(
        "over a socket, to a handler that waits once",
        [&](net::io::scheduler* /*sched*/) { return waiting; },
        waiting_request);
    measure_http2(router);
    measure("pipelined, through http_metrics",
            http::chain(net::instrument::http_metrics{}, router),
            [](std::string_view input) { return net::io::string_reader{input}; });

    // NOTE: a lambda rather than plaintext itself, whose pointer would be called indirectly: its frame not elided.
    const auto on_route = http::make_static_router(
        http::use(net::instrument::http_metrics{}),
        http::GET("/plaintext"_route,
                  [](const http::server_request& req, http::response_writer& resp) { return plaintext(req, resp); }));
    measure("pipelined, http_metrics on the route",
            on_route,
            [](std::string_view input) { return net::io::string_reader{input}; });

    http::router runtime;
    runtime.GET("/plaintext", plaintext);
    measure("pipelined, the runtime router",
            runtime,
            [](std::string_view input) { return net::io::string_reader{input}; });

    runtime.use(net::instrument::http_metrics{});
    measure("pipelined, the runtime router, http_metrics on the route",
            runtime,
            [](std::string_view input) { return net::io::string_reader{input}; });

    // NOTE: a deadline that never comes, for what the timer costs (on the route, so the layer's frame is elided):
    //       against "pipelined" and "over a socket, a request at a time", which have the same handler.
    net::io::scheduler timers{std::make_shared<net::coro::thread_pool>(2)};

    const auto timed = [](net::io::scheduler* sched)
    {
        return http::make_static_router(http::use(http::timeout{sched, 60s}),
                                        http::GET("/plaintext"_route,
                                                  [](const http::server_request& req, http::response_writer& resp)
                                                  { return plaintext(req, resp); }));
    };
    measure("pipelined, http::timeout on the route",
            timed(&timers),
            [](std::string_view input) { return net::io::string_reader{input}; });
    measure_socket("over a socket, http::timeout on the route", timed, request);
}
