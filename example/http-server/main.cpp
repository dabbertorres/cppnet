#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <pthread.h>

#include <spdlog/common.h>
#include <spdlog/logger.h>
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

namespace http = net::http;

int main()
{
    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    auto logger = spdlog::default_logger()->clone("default");
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("[%T.%e] [%^%-5!l%$] [%-11!n] [t %-8!t] %v");
    spdlog::set_default_logger(logger);

    http::server_config config{
        .logger = logger,
    };
    http::router router;
    router.GET("/",
               [](const http::server_request& /*req*/, http::response_writer& resp) -> net::coro::task<void>
               {
                   resp.headers().set("X-Msg"sv, "Hello"sv).set("Content-Type"sv, "text/plain"sv);
                   constexpr auto msg = "hello world\r\n"sv;

                   auto* body = co_await resp.send(http::status::OK, msg.length());
                   co_await body->write(msg);
               });

    // Block the signals before starting any threads (which inherit the mask), so that only sigwait() receives them.
    sigset_t sig_set{};
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGINT);
    sigaddset(&sig_set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sig_set, nullptr);

    auto workers = std::make_shared<net::coro::thread_pool>(net::coro::hardware_concurrency(1));

    net::io::scheduler scheduler{workers, logger};

    http::server server{&scheduler, std::move(router), config};

    config.logger->info("starting...");
    scheduler.spawn(server.serve());

    auto run_thread = std::thread{[&] { scheduler.run(); }};

    int sig = 0;
    int ret = sigwait(&sig_set, &sig);
    if (ret != 0)
    {
        spdlog::critical("error in sigwait(): {}; errno = {}", ret, errno);
        std::abort();
    }

    config.logger->info("exiting...");

    // stop accepting connections, then cancel the ones still open, and wait for them to finish.
    server.close();
    scheduler.shutdown();

    if (run_thread.joinable()) run_thread.join();

    return 0;
}
