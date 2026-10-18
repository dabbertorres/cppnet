// http-server serves "hello world" on port 8080, and "hello, <name>" at /hello/<name>, until SIGINT or SIGTERM. Then it
// lets the requests in progress finish, for up to 10 seconds, before it stops them.
//
// With --cert <chain.pem> --key <key.pem> (if built with TLS), it serves HTTPS, with that certificate chain and key.

#include <chrono>
#include <csignal>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <pthread.h>

#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/headers.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "listen.hpp"

#ifdef NET_HAS_TLS
#    include "tls/tls.hpp"
#endif

namespace http = net::http;

using net::coro::task;

namespace
{

// abort_after aborts server after grace, unless stop is requested first.
task<>
abort_after(net::io::scheduler* scheduler, http::server* server, std::chrono::seconds grace, std::stop_token stop)
{
    // canceled (once the server's drained, or the scheduler's stopping).
    if (co_await scheduler->sleep_until(net::io::clock::now() + grace, std::move(stop))) co_return;

    spdlog::warn("requests still in progress after {}s: stopping them", grace.count());
    server->abort();
}

// drain waits for server's connections to finish, once it's closed: for up to grace, after which it aborts it (stopping
// its handlers' tokens), and waits for the handlers to return.
task<> drain(net::io::scheduler* scheduler, http::server* server, std::chrono::seconds grace)
{
    std::stop_source drained;
    scheduler->spawn(abort_after(scheduler, server, grace, drained.get_token()));

    co_await server->join();
    drained.request_stop();
}

}

int main(int argc, char** argv)
{
    using namespace std::string_view_literals;

    // --cert and --key: the certificate chain and private key to serve HTTPS with.
    std::string_view cert;
    std::string_view key;

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i + 1 < args.size(); i += 2)
    {
        if (args[i] == "--cert"sv) cert = args[i + 1];
        else if (args[i] == "--key"sv) key = args[i + 1];
    }

    if (cert.empty() != key.empty() || args.size() % 2 != 0)
    {
        spdlog::critical("usage: {} [--cert <chain.pem> --key <key.pem>]", argv[0]);
        return 2;
    }

    http::server_config config;
    if (!cert.empty())
    {
#ifdef NET_HAS_TLS
        // loads the certificate chain and key now, so a problem with them is reported before serving.
        auto ctx = net::tls::server_context::make({.certificate_chain = cert, .private_key = key});
        if (!ctx.has_value())
        {
            spdlog::critical("can't load the certificate or key: {}", ctx.error().message());
            return 1;
        }

        config.tls = std::move(*ctx);
#else
        spdlog::critical("built without TLS (NET_TLS is off)");
        return 1;
#endif
    }

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

    http::server server{&scheduler, std::move(*listener), std::move(router), config};
    scheduler.spawn(server.serve());
    spdlog::info("listening on port 8080{}", cert.empty() ? "" : ", for HTTPS");

    // the scheduler's workers serve the connections: this thread only waits for a signal.
    int sig = 0;
    if (int err = sigwait(&signals, &sig); err != 0)
    {
        spdlog::critical("sigwait() failed: {}", err);
        return 1;
    }

    spdlog::info("exiting...");

    // stop accepting connections, and end the ones still open: gracefully, then (after a while) not.
    server.close();
    scheduler.run_to_completion(drain(&scheduler, &server, std::chrono::seconds{10}));

    return 0;
}
