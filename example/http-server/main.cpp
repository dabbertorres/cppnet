// http-server serves "hello world" on port 8080, and "hello, <name>" at /hello/<name>, until SIGINT or SIGTERM.

#include <cerrno>
#include <csignal>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <pthread.h>

#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "listen.hpp"

namespace http = net::http;

using net::coro::task;

int main()
{
    using namespace std::string_view_literals;

    // Block the signals before starting any threads (which inherit the mask), so that only sigwait() receives them.
    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    // the runtime: worker threads, and a scheduler that runs tasks on them and waits for their I/O.
    net::io::scheduler scheduler{std::make_shared<net::coro::thread_pool>()};

    http::router router;
    router.GET("/",
               [](const http::server_request&, http::response_writer& resp) -> task<void>
               {
                   constexpr auto msg = "hello world\n"sv;

                   resp.headers().set("Content-Type", "text/plain");
                   auto* body = co_await resp.send(http::status::OK, msg.size());
                   co_await net::io::write_all(*body, msg);
               });
    router.GET("/hello/:name",
               [](const http::server_request& req, http::response_writer& resp) -> task<void>
               {
                   const auto msg = std::format("hello, {}\n", req.path_value("name"));

                   resp.headers().set("Content-Type", "text/plain");
                   auto* body = co_await resp.send(http::status::OK, msg.size());
                   co_await net::io::write_all(*body, msg);
               });

    auto listener = net::listener::make(&scheduler, "", "8080");
    if (!listener.has_value())
    {
        spdlog::critical("can't listen on port 8080: {}", listener.error().message());
        return 1;
    }

    http::server server{&scheduler, std::move(*listener), std::move(router)};
    scheduler.spawn(server.serve());
    spdlog::info("listening on port 8080");

    // the scheduler's workers serve the connections: this thread only waits for a signal.
    int sig = 0;
    if (int err = sigwait(&signals, &sig); err != 0)
    {
        spdlog::critical("sigwait() failed: {}", err);
        return 1;
    }

    spdlog::info("exiting...");

    // stop accepting connections, end the ones still open, and wait for them to finish.
    server.close();
    scheduler.shutdown();

    return 0;
}
