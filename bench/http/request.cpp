// bench_request measures what the server does for each request, apart from the socket: decoding it (HTTP/1.1), routing
// it to a handler, the handler's response, and encoding that, with a stream of TechEmpower's plaintext requests (as a
// client pipelining them sends them, read from memory rather than a socket).
//
// It also counts heap allocations per request, at each of those steps.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <print>
#include <ratio>
#include <string>
#include <string_view>
#include <vector>

#include "coro/task.hpp"
#include "http/http11.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/static_router.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/discard_writer.hpp"
#include "io/string_reader.hpp"
#include "io/writer.hpp"

// NOLINTBEGIN(misc-new-delete-overloads,cppcoreguidelines-no-malloc,cppcoreguidelines-avoid-non-const-global-variables,readability-inconsistent-declaration-parameter-name):
// counting allocations, by replacing these.
namespace
{

// the heap allocations so far (on any thread, though the benchmark only uses one).
std::size_t allocations = 0;

}

void* operator new(std::size_t size)
{
    ++allocations;
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

constexpr std::size_t pipelined  = 16;
constexpr int         batches    = 50'000;
constexpr int         repeats    = 7;
constexpr std::size_t date_bytes = 29; // e.g. "Sun, 06 Nov 1994 08:49:37 GMT"

task<void> plaintext(const http::server_request& /*req*/, http::response_writer& resp)
{
    constexpr auto body = "Hello, World!"sv;

    resp.headers().set("Server"sv, "cppnet"sv).set("Content-Type"sv, "text/plain"sv);
    auto* out = co_await resp.send(http::status::OK, body.size());
    co_await net::io::write_all(*out, body);
}

// steps counts allocations at each step of serving a request.
struct steps
{
    std::size_t decode   = 0;
    std::size_t handle   = 0; // routing, the handler, and encoding its response
    std::size_t finish   = 0; // the rest: e.g. the server's own headers, and skipping the request's body
    std::size_t requests = 0;
};

// serve serves the requests in input, as server::serve_request does (without its logging and error handling).
template<typename Router>
task<void> serve(const Router& router, std::string_view input, net::io::writer& out, steps& counted)
{
    net::io::string_reader   in{input};
    net::io::buffered_reader reader{&in, std::size_t{16} * 1'024};
    net::io::buffered_writer writer{&out, std::size_t{16} * 1'024};

    const std::string date(date_bytes, 'x');

    while (true)
    {
        auto at = allocations;

        auto decoded = co_await http::http11::request_decode(&reader, 8'192);
        if (!decoded.has_value()) break;

        counted.decode += allocations - at;
        at = allocations;

        auto& req = *decoded;

        http::server_response resp{
            .version = req.version,
            .headers = {},
            .body    = &writer,
        };
        resp.headers.set("Date"sv, std::string{date});

        http::response_writer rw{&writer, &resp, http::http11::response_encode};

        counted.finish += allocations - at;
        at = allocations;

        co_await router(req, rw);

        counted.handle += allocations - at;
        at = allocations;

        // skips what's left of the body (none, here).
        std::array<std::byte, 64> scratch{};
        while (!(co_await req.body->read(scratch)).err) {}

        if (reader.size() == 0) co_await writer.flush();

        counted.finish += allocations - at;
        ++counted.requests;
    }
}

}

int main()
{
    const auto router = http::make_static_router(http::GET("/plaintext"_route, plaintext));

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
            auto served = serve(router, input, out, counted);
            served.get_handle().resume();
        }

        auto took = std::chrono::steady_clock::now() - start;
        if (counted.requests != pipelined * batches) std::println(stderr, "served {} requests", counted.requests);

        runs.push_back(std::chrono::duration<double, std::nano>(took).count() / static_cast<double>(counted.requests));
    }

    std::ranges::sort(runs);

    // and once more, counting allocations at each step.
    steps counted;
    auto  served = serve(router, input, out, counted);
    served.get_handle().resume();

    const auto per_request = [&](std::size_t n) { return static_cast<double>(n) / static_cast<double>(counted.requests); };

    std::println("{:>10.1f} ns per request (best of {})", runs.front(), repeats);
    std::println("{:>10.1f} allocations per request: decoding {:.1f}, handling {:.1f}, the rest {:.1f}",
                 per_request(counted.decode + counted.handle + counted.finish),
                 per_request(counted.decode),
                 per_request(counted.handle),
                 per_request(counted.finish));
}
