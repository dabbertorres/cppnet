// bench_request measures what the server does for each request: decoding it (HTTP/1.1), routing it to a handler, the
// handler's response, and encoding that, with TechEmpower's plaintext requests. It counts heap allocations per request
// too. Three ways:
// - pipelined: 16 requests read together (as a client pipelining them sends them), from memory rather than a socket.
// - one per read: the same, but each read hands over just one request, and has to wait for it (a coroutine, as a
//   socket's read that has to wait is): as for a client that waits for each response, apart from the socket.
// - over a socket: the server itself, and a client on another thread sending a request at a time (as TechEmpower's
//   JSON test does), so with the socket, scheduler and kernel's round trip too.
#include "http/request.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/response.hpp"
#include "http/server.hpp"
#include "http/static_router.hpp"
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
using net::coro::task;

// as bench/web/cppnet's, and wrk sends it, with TechEmpower's headers.
constexpr auto request = "GET /plaintext HTTP/1.1\r\n"
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

task<void> plaintext(const http::server_request& /*req*/, http::response_writer& resp)
{
    resp.headers().set("Server"sv, "cppnet"sv).set("Content-Type"sv, "text/plain"sv);
    auto* out = co_await resp.send(http::status::OK, body.size());
    co_await net::io::write_all(*out, body);
}

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

    [[nodiscard]] int native_handle() const noexcept override { return -1; }

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
        while (!(co_await req.body->read(scratch)).err) {}

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

    const auto per_request = [&](std::size_t n) { return static_cast<double>(n) / static_cast<double>(counted.requests); };

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
    explicit client(std::uint16_t port)
        : fd{::socket(AF_INET, SOCK_STREAM, 0)}
    {
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

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
    std::array<char, 4'096> buf{};
};

// measure_socket serves requests over a socket, a request at a time, and prints the time and allocations per request.
template<typename Router>
void measure_socket(const Router& router)
{
    auto               workers = std::make_shared<net::coro::thread_pool>(2);
    net::io::scheduler scheduler{workers};

    http::server server{
        &scheduler,
        router,
        {.host = "127.0.0.1", .port = "0"}
    };

    scheduler.spawn(server.serve());
    std::thread runner{[&] { scheduler.run(); }};

    {
        client conn{server.local_port()};

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

        std::println("over a socket (a request at a time, {} workers):", workers->concurrency());
        std::println("{:>10.1f} us per request",
                     std::chrono::duration<double, std::micro>(took).count() / socket_requests);
        std::println("{:>10.1f} allocations per request",
                     static_cast<double>(counted) / static_cast<double>(socket_requests));
    }

    server.close();
    scheduler.shutdown();
    runner.join();
}

}

int main()
{
    spdlog::set_level(spdlog::level::warn);

    const auto router = http::make_static_router(http::GET("/plaintext"_route, plaintext));

    measure("pipelined", router, [](std::string_view input) { return net::io::string_reader{input}; });
    measure("one per read", router, [](std::string_view input) { return one_request_per_read{input}; });
    measure_socket(router);
}
