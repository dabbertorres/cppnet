// bench_web is cppnet's implementation of the TechEmpower Framework Benchmarks' plaintext and JSON tests, which
// bench/web/run.sh compares with other servers' (see its comments).
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <pthread.h>

#include <glaze/glaze.hpp>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/server.hpp"
#include "http/static_router.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"

// message is the JSON test's object, which glaze serializes by reflection (as serde does Rust's structs).
// NOTE: not in the anonymous namespace: glaze's reflection needs it to have linkage.
namespace tfb
{

struct message // NOLINT(misc-use-internal-linkage): see above.
{
    std::string_view message;
};

}

namespace
{

namespace http = net::http;

using http::literals::operator""_route;
using namespace std::string_view_literals;
using net::coro::task;

task<void> plaintext(const http::server_request& /*req*/, http::response_writer& resp)
{
    constexpr auto body = "Hello, World!"sv;

    resp.headers().set("Server"sv, "cppnet"sv).set("Content-Type"sv, "text/plain"sv);
    auto* out = co_await resp.send(http::status::OK, body.size());
    co_await net::io::write_all(*out, body);
}

task<void> json(const http::server_request& /*req*/, http::response_writer& resp)
{
    // NOTE: serialized for each request, as the benchmark requires.
    std::string body;
    if (glz::write_json(tfb::message{.message = "Hello, World!"}, body))
    {
        co_await resp.send(http::status::INTERNAL_SERVER_ERROR, 0);
        co_return;
    }

    resp.headers().set("Server"sv, "cppnet"sv).set("Content-Type"sv, "application/json"sv);
    auto* out = co_await resp.send(http::status::OK, body.size());
    co_await net::io::write_all(*out, body);
}

}

int main()
{
    spdlog::set_level(spdlog::level::warn);

    const char* port = std::getenv("PORT"); // NOLINT(concurrency-mt-unsafe): before any threads.

    // Block the signals before starting any threads (which inherit the mask), so that only sigwait() receives them.
    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    auto               workers = std::make_shared<net::coro::thread_pool>(net::coro::hardware_concurrency(1));
    net::io::scheduler scheduler{workers};

    const auto router =
        http::make_static_router(http::GET("/plaintext"_route, plaintext), http::GET("/json"_route, json));

    http::server server{
        &scheduler,
        router,
        {.port = port != nullptr ? port : "8080", .max_pending_connections = 4'096}
    };

    scheduler.spawn(server.serve());
    std::thread runner{[&] { scheduler.run(); }};

    int signal = 0;
    if (sigwait(&signals, &signal) != 0) std::abort();

    server.close();
    scheduler.shutdown();
    runner.join();
    return 0;
}
