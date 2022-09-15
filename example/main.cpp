#include <algorithm>
#include <iostream>

#include "http/http.hpp"
#include "http/router.hpp"
#include "http/server.hpp"

int main(int argc, char** argv)
{
    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    net::http::server server{
        "8080",
        {
          .max_header_bytes        = 4096,
          .header_read_timeout     = 5s,
          .max_pending_connections = 64,
          }
    };

    std::cout << "starting...\n";
    server.serve(
        [](const net::http::request& req, net::http::server_response& resp)
        {
            resp.status_code = net::http::status::OK;
            resp.headers.set("X-Msg"sv, "Hello"sv);
            resp.body->write("hello world", 11);
            std::cout << "handled: " << net::http::method_string(req.method) << " " << req.uri.build() << '\n';
        });
    std::cout << "exiting...\n";

    return 0;
}
