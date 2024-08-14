#include <csignal>
#include <iostream>
#include <string_view>
#include <utility>

#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/buffer.hpp"
#include "io/scheduler.hpp"

#include "instrument/prometheus/histogram.hpp"

namespace http = net::http;

int main()
{
    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    spdlog::set_level(spdlog::level::trace);

    net::instrument::prometheus::histogram request_times{"request_latency"};

    http::server_config config{};
    http::router        router;
    router.GET("/",
               [&](const http::server_request& req, http::response_writer& resp) -> net::coro::task<void>
               {
                   auto tracker = request_times.track();

                   resp.headers().set("X-Msg"sv, "Hello"sv).set("Content-Type"sv, "text/plain"sv);
                   constexpr auto msg = "hello world\r\n"sv;

                   auto body = co_await resp.send(http::status::OK, msg.length());
                   co_await body->co_write(msg);
               });

    net::io::scheduler scheduler;

    http::server server{&scheduler, std::move(router), config};

    config.logger->info("starting...");
    auto serve_task = server.serve();

    scheduler.run();
    config.logger->info("exiting...");

    net::io::buffer buf;
    request_times.encode(buf);

    std::cout << buf.to_string() << '\n';

    return 0;
}
