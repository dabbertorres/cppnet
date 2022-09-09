#include <algorithm>
#include <iostream>

#include "http/http.hpp"
#include "http/router.hpp"
#include "http/server.hpp"

int main(int argc, char** argv)
{
    using namespace std::chrono_literals;

    net::http::server server{
        "8080",
        {
          .max_header_bytes        = 4096,
          .header_read_timeout     = 5s,
          .max_pending_connections = 64,
          }
    };

    server.serve(
        [](const net::http::request& req, net::http::response& resp)
        { std::cout << "handled: " << net::http::method_string(req.method) << " " << req.uri.build() << '\n'; });

    return 0;
}
