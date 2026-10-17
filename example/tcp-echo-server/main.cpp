// tcp-echo-server echoes what each connection sends on port 7777, until SIGINT or SIGTERM.

#include <array>
#include <csignal>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <pthread.h>

#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace
{

net::coro::task<> serve_connection(net::tcp_socket socket)
{
    spdlog::info("serving new connection from {} on {}", socket.remote_addr(), socket.local_addr());

    std::array<std::byte, 1024> buf{};

    while (true)
    {
        auto res = co_await socket.read(buf);
        if (res.err) co_return;

        res = co_await socket.write(std::span{buf}.subspan(0, res.count));
        if (res.err) co_return;
    }
}

net::coro::task<> run_echo_server(net::io::scheduler& scheduler, const net::listener& listener)
{
    while (true)
    {
        auto socket = co_await listener.accept();
        if (!socket.has_value())
        {
            if (socket.error() == std::errc::operation_canceled) co_return;

            spdlog::warn("error accepting connection: {}", socket.error().message());
            continue;
        }

        if (!scheduler.spawn(serve_connection(std::move(*socket)))) co_return;
    }
}

}

int main()
{
    // Block the signals before starting any threads (which inherit the mask), so that only sigwait() receives them.
    sigset_t signals{};
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    // the runtime: worker threads, and a scheduler that runs tasks on them and waits for their I/O.
    net::io::scheduler scheduler{std::make_shared<net::coro::thread_pool>()};

    auto listener = net::listener::make(&scheduler, "", "7777");
    if (!listener.has_value())
    {
        spdlog::critical("can't listen on port 7777: {}", listener.error().message());
        return 1;
    }

    scheduler.spawn(run_echo_server(scheduler, *listener));
    spdlog::info("echoing on port 7777");

    // the scheduler's workers serve the connections: this thread only waits for a signal.
    int sig = 0;
    if (int err = sigwait(&signals, &sig); err != 0)
    {
        spdlog::critical("sigwait() failed: {}", err);
        return 1;
    }

    spdlog::info("shutting down...");

    // stop accepting, and cancel what the connections are waiting for (ending them), then wait for them to finish.
    listener->shutdown();
    scheduler.shutdown();

    return 0;
}
