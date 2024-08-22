#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <memory>
#include <span>
#include <utility>

#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"

net::coro::task<> serve_connection(net::tcp_socket socket)
{
    spdlog::info("serving new connection from {} on {}", socket.local_addr(), socket.remote_addr());

    std::array<std::byte, 1024> buf{};

    while (socket.valid())
    {
        spdlog::info("reading from socket {}", socket.remote_addr());

        auto res = co_await socket.read(buf);
        if (res.err) co_return;

        spdlog::info("writing to socket {}", socket.remote_addr());

        res = co_await socket.write(std::span{buf}.subspan(0, res.count));
        if (res.err) co_return;
    }
}

net::coro::task<> run_echo_server(net::io::scheduler& scheduler, net::listener& listener, std::atomic<bool>& run)
{
    listener.listen(512);

    while (run)
    {
        try
        {
            auto socket = co_await listener.accept();
            if (!socket.valid()) co_return;

            spdlog::info("accepted new connection from {} on {}", socket.local_addr(), socket.remote_addr());

            scheduler.schedule(serve_connection(std::move(socket)));
        }
        catch (const std::exception& ex)
        {
            spdlog::warn("error accepting connection: {}", ex.what());
        }
    }
}

int main()
{
    auto logger = spdlog::default_logger()->clone("default");
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("[%T.%e] [%^%-5!l%$] [%-11!n] [t %-8!t] %v");
    spdlog::set_default_logger(logger);

    sigset_t sig_set;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGINT);
    sigaddset(&sig_set, SIGTERM);

    auto workers = std::make_shared<net::coro::thread_pool>(net::coro::hardware_concurrency(1), logger);

    net::io::scheduler scheduler{workers, logger};
    net::listener      listener{&scheduler, "7777", net::network::tcp};

    std::atomic<bool> running = true;

    spdlog::info("starting...");
    scheduler.schedule(run_echo_server(scheduler, listener, running));

    int sig;
    int ret = sigwait(&sig_set, &sig);
    if (ret != 0)
    {
        spdlog::critical("error in sigwait(): {}; errno = {}", ret, errno);
        std::abort();
    }

    spdlog::info("shutting down...");

    running = false;
    listener.shutdown();
    scheduler.shutdown();

    spdlog::info("done");

    return 0;
}
