#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <utility>

#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "instrument/prometheus/histogram.hpp"
#include "io/buffer.hpp"
#include "io/scheduler.hpp"

namespace http = net::http;

int main()
{
    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    auto logger = spdlog::default_logger()->clone("default");
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("[%T.%e] [%^%-5!l%$] [%-11!n] [t %-8!t] %v");
    spdlog::set_default_logger(logger);

    net::instrument::prometheus::histogram request_times{"request_latency"};

    http::server_config config{
        .logger = logger,
    };
    http::router router;
    router.GET("/",
               [&](const http::server_request& req, http::response_writer& resp) -> net::coro::task<void>
               {
                   auto tracker = request_times.track();

                   resp.headers().set("X-Msg"sv, "Hello"sv).set("Content-Type"sv, "text/plain"sv);
                   constexpr auto msg = "hello world\r\n"sv;

                   auto* body = co_await resp.send(http::status::OK, msg.length());
                   co_await body->write(msg);
               });

    sigset_t sig_set;
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGINT);
    sigaddset(&sig_set, SIGTERM);

    auto workers = std::make_shared<net::coro::thread_pool>(net::coro::hardware_concurrency(1), logger);

    net::io::scheduler scheduler{workers, logger};

    http::server server{&scheduler, std::move(router), config};

    config.logger->info("starting...");
    scheduler.schedule(server.serve());

    int sig;
    int ret = sigwait(&sig_set, &sig);
    if (ret != 0)
    {
        spdlog::critical("error in sigwait(): {}; errno = {}", ret, errno);
        std::abort();
    }

    config.logger->info("exiting...");

    server.close();
    scheduler.shutdown();

    net::io::buffer buf;
    scheduler.schedule(request_times.encode(buf));

    std::cout << buf.to_string() << '\n';

    return 0;
}
