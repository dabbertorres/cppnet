// websocket-echo accepts WebSockets on port 9001 (Autobahn's TestSuite's), at /, and echoes each message back
// whole, until the client closes, or SIGINT or SIGTERM: then it closes the server, which tells each WebSocket it's
// going away (a close with 1001, as each is reading), and waits for them to end (their clients answering, or not within
// close_timeout, 5 seconds), for up to 10 seconds, before it stops them. Messages may be up to 16 MiB.
//
// --port <port> listens on another port. With --cert <chain.pem> --key <key.pem> (if built with TLS), it serves wss://,
// with that certificate chain and key.
//
// e.g. websocat ws://127.0.0.1:9001/

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pthread.h>

#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "websocket/websocket.hpp"

#ifdef NET_HAS_TLS
#    include "tls/tls.hpp"
#endif

namespace http = net::http;
namespace ws   = net::websocket;

using net::coro::task;

namespace
{

// echo accepts the request's WebSocket, and echoes its messages until it ends.
task<> echo(const http::server_request& req, http::response_writer& resp)
{
    // any origin: this server authenticates no one, so a page elsewhere gains nothing by opening one.
    auto conn =
        co_await ws::accept(req,
                            resp,
                            {.insecure_any_origin = true, .connection = {.max_message = std::uint64_t{16} << 20}});
    if (!conn.has_value())
    {
        spdlog::debug("refused a handshake: {}", conn.error().message());
        co_return;
    }

    std::string message;
    while (true)
    {
        // the client's close ends the read (answered already), as does a failure (the client told why, if it was its),
        // and the server's close (going away, once the client has answered).
        const auto type = co_await conn->read_message(message);
        if (!type.has_value())
        {
            spdlog::debug("WebSocket ended: {}", type.error().message());
            co_return;
        }

        if (auto err = co_await conn->write_message(*type, message))
        {
            spdlog::debug("WebSocket failed: {}", err.message());
            co_return;
        }
    }
}

// abort_after aborts server after grace, unless stop is requested first.
task<>
abort_after(net::io::scheduler* scheduler, http::server* server, std::chrono::seconds grace, std::stop_token stop)
{
    if (co_await scheduler->sleep_until(net::io::clock::now() + grace, std::move(stop))) co_return;

    spdlog::warn("WebSockets still open after {}s: stopping them", grace.count());
    server->abort();
}

// drain waits for server's connections to end, once it's closed, for up to grace: then it aborts them.
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

    std::string_view port = "9001";
    std::string_view cert;
    std::string_view key;

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i + 1 < args.size(); i += 2)
    {
        if (args[i] == "--port"sv) port = args[i + 1];
        else if (args[i] == "--cert"sv) cert = args[i + 1];
        else if (args[i] == "--key"sv) key = args[i + 1];
    }

    if (cert.empty() != key.empty() || args.size() % 2 != 0)
    {
        spdlog::critical("usage: {} [--port <port>] [--cert <chain.pem> --key <key.pem>]", argv[0]);
        return 2;
    }

    http::server_config config;
    if (!cert.empty())
    {
#ifdef NET_HAS_TLS
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

    net::io::scheduler scheduler{std::make_shared<net::coro::thread_pool>()};

    // a WebSocket is a route like any other.
    http::router router;
    router.GET("/", echo);

    auto listener = net::listener::make(&scheduler, "", std::string{port});
    if (!listener.has_value())
    {
        spdlog::critical("can't listen on port {}: {}", port, listener.error().message());
        return 1;
    }

    http::server server{&scheduler, std::move(*listener), std::move(router), config};
    scheduler.spawn(server.serve());
    spdlog::info("echoing WebSockets on port {}{}", port, cert.empty() ? "" : ", over TLS");

    int sig = 0;
    if (int err = sigwait(&signals, &sig); err != 0)
    {
        spdlog::critical("sigwait() failed: {}", err);
        return 1;
    }

    spdlog::info("exiting...");

    // stop accepting, then wait for the WebSockets to end: their reads close them, going away.
    server.close();
    scheduler.run_to_completion(drain(&scheduler, &server, std::chrono::seconds{10}));

    return 0;
}
