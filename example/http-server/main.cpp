#include <iostream>
#include <string_view>
#include <utility>

#include <spdlog/common.h>

#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/scheduler.hpp"

namespace http = net::http;

int main()
{
    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    http::server_config config{};
    http::router        router;
    router.add().use(
        [](const http::server_request& req, http::response_writer& resp)
        {
            resp.headers().set("X-Msg"sv, "Hello"sv).set("Content-Type"sv, "text/plain"sv);
            resp.send(http::status::OK, 11).write("hello world"sv);
            std::cout << "handled: " << http::method_string(req.method) << " " << req.uri.build() << '\n';
        });

    config.logger->set_level(spdlog::level::trace);

    net::io::scheduler scheduler;

    http::server server{&scheduler, std::move(router), config};

    std::cout << "starting...\n";
    auto serve_task = server.serve();
    std::cout << "exiting...\n";

    return 0;
}
