// bench_router compares routing a request with router (built at runtime, as a tree of hash maps) and static_router
// (whose patterns are template arguments, so that its tree is built at compile time, as tables with a perfect hash
// for each node's literal segments, and handlers are called through a table of function pointers).
//
// Each request is routed to a handler that does nothing, so the time is the router's: matching the path (decoding
// its segments, and collecting parameters) and calling the handler (whose coroutine frame is allocated either way).
// It also counts heap allocations per request: those more than without a router are the router's.
#include "http/router.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <print>
#include <ratio>
#include <string>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "http/http11.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/static_router.hpp"
#include "io/discard_writer.hpp"

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
using net::coro::task;

task<void> nothing(const http::server_request& /*req*/, http::response_writer& /*resp*/) { co_return; }

constexpr int iterations = 1'000'000;
constexpr int repeats    = 7;

struct result
{
    double nanoseconds = 0; // per request, the best of the runs
    double allocations = 0; // per request
};

template<typename Router>
result measure(const Router& router, const std::vector<std::pair<http::request_method, std::string>>& requests)
{
    net::io::discard_writer out;
    std::vector<double>     runs;

    for (int r = 0; r < repeats; ++r)
    {
        auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < iterations; ++i)
        {
            const auto& [method, path] = requests[static_cast<std::size_t>(i) % requests.size()];

            http::server_request req{.method = method};
            req.target.path = path;

            http::server_response resp{
                .version = {.major = 1, .minor = 1},
                .body    = &out
            };
            http::response_writer rw{&out, &resp, http::http11::response_encode};

            auto routed = router(req, rw);
            routed.get_handle().resume();
            if (rw.status_code() != http::status::NONE) std::println(stderr, "unrouted: {}", path);
        }

        auto took = std::chrono::steady_clock::now() - start;
        runs.push_back(std::chrono::duration<double, std::nano>(took).count() / iterations);
    }

    std::ranges::sort(runs);

    // and once more, counting allocations.
    const auto before = allocations;
    for (const auto& [method, path] : requests)
    {
        http::server_request req{.method = method};
        req.target.path = path;

        http::server_response resp{
            .version = {.major = 1, .minor = 1},
            .body    = &out
        };
        http::response_writer rw{&out, &resp, http::http11::response_encode};

        auto routed = router(req, rw);
        routed.get_handle().resume();
    }

    return {.nanoseconds = runs.front(),
            .allocations = static_cast<double>(allocations - before) / static_cast<double>(requests.size())};
}

void print(const char* name, result r)
{
    std::println("{:14} best {:7.1f} ns per request, {:4.1f} allocations per request",
                 name,
                 r.nanoseconds,
                 r.allocations);
}
}

int main()
{
#ifndef __OPTIMIZE__
    std::println(stderr, "bench_router needs an optimized build (e.g. --config Release)");
    return 1;
#endif

    // the requests: literal routes, and routes with parameters, some with encoded characters.
    const std::vector<std::pair<http::request_method, std::string>> requests{
        {   http::request_method::GET,                          "/"},
        {   http::request_method::GET,                    "/health"},
        {   http::request_method::GET,              "/api/v1/users"},
        {   http::request_method::GET,           "/api/v1/users/42"},
        {  http::request_method::POST,     "/api/v1/users/42/posts"},
        {   http::request_method::GET,   "/api/v1/users/42/posts/7"},
        {http::request_method::DELETE, "/api/v1/users/me%40example"},
        {   http::request_method::GET,  "/api/v1/orgs/acme/members"},
        {   http::request_method::GET,       "/static/css/site.css"},
        {   http::request_method::GET,             "/api/v2/search"},
    };

    http::router dynamic;
    dynamic.GET("/", nothing)
        .GET("/health", nothing)
        .subrouter("/api/v1",
                   [](http::router& api)
                   {
                       api.GET("/users", nothing)
                           .GET("/users/:id", nothing)
                           .DELETE("/users/:id", nothing)
                           .POST("/users/:id/posts", nothing)
                           .GET("/users/:id/posts/:post", nothing)
                           .GET("/orgs/:org/members", nothing);
                   })
        .GET("/static/css/site.css", nothing)
        .GET("/api/v2/search", nothing);

    const auto fixed = http::make_static_router(http::GET("/"_route, nothing),
                                                http::GET("/health"_route, nothing),
                                                http::group("/api/v1"_route,
                                                            http::GET("/users"_route, nothing),
                                                            http::GET("/users/:id"_route, nothing),
                                                            http::DELETE("/users/:id"_route, nothing),
                                                            http::POST("/users/:id/posts"_route, nothing),
                                                            http::GET("/users/:id/posts/:post"_route, nothing),
                                                            http::GET("/orgs/:org/members"_route, nothing)),
                                                http::GET("/static/css/site.css"_route, nothing),
                                                http::GET("/api/v2/search"_route, nothing));

    // the same, without routing: what making the request costs, and calling a handler.
    const auto direct = [](http::server_request& req, http::response_writer& resp) { return nothing(req, resp); };

    print("no router", measure(direct, requests));
    print("router", measure(dynamic, requests));
    print("static_router", measure(fixed, requests));
    return 0;
}
