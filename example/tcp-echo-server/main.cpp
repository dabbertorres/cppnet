#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

#include <pthread.h>

#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
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

net::coro::task<> run_echo_server(net::io::scheduler& scheduler, net::listener& listener)
{
    listener.listen(512);

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
    auto logger = spdlog::default_logger()->clone("default");
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("[%T.%e] [%^%-5!l%$] [%-11!n] [t %-8!t] %v");
    spdlog::set_default_logger(logger);

    // Block the signals before starting any threads (which inherit the mask), so that only sigwait() receives them.
    sigset_t sig_set;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGINT);
    sigaddset(&sig_set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sig_set, nullptr);

    auto workers = std::make_shared<net::coro::thread_pool>(net::coro::hardware_concurrency(1));

    net::io::scheduler scheduler{workers, logger};
    net::listener      listener{&scheduler, "7777", net::network::tcp};

    spdlog::info("starting...");
    scheduler.spawn(run_echo_server(scheduler, listener));

    auto run_thread = std::thread{[&] { scheduler.run(); }};

    int sig;
    int ret = sigwait(&sig_set, &sig);
    if (ret != 0)
    {
        spdlog::critical("error in sigwait(): {}; errno = {}", ret, errno);
        std::abort();
    }

    spdlog::info("shutting down...");

    listener.shutdown();
    scheduler.shutdown();

    if (run_thread.joinable()) run_thread.join();

    spdlog::info("done");

    return 0;
}
