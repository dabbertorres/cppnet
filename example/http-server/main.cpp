#include <algorithm>
#include <iostream>

#include "http/server.hpp"

int main()
{
    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    net::http::server_config config{};
    config.router.add().use(
        [](const net::http::server_request& req, net::http::response_writer& resp)
        {
            resp.headers().set("X-Msg"sv, "Hello"sv);
            resp.send(net::http::status::OK, 11).write("hello world", 11);
            std::cout << "handled: " << net::http::method_string(req.method) << " " << req.uri.build() << '\n';
        });
    config.logger->set_level(spdlog::level::trace);

    net::http::server server{config};

    std::cout << "starting...\n";
    server.serve();
    std::cout << "exiting...\n";

    return 0;
}
