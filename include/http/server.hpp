#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "http/router.hpp"

#include "listen.hpp"

namespace net::http
{

struct server_config
{
    size_t               max_header_bytes;
    std::chrono::seconds header_read_timeout;
    uint16_t             max_pending_connections;
    // TODO: logging
    // TODO: threading
    // TODO: coroutines
};

class server
{
public:
    server(const std::string& port, const server_config& cfg);
    server(const std::string& host, const std::string& port, const server_config& cfg);

    server(const server&)            = delete;
    server& operator=(const server&) = delete;

    server(server&&) noexcept;
    server& operator=(server&&) noexcept;

    ~server() = default;

    void close();
    void serve(const handler&) const;
    void serve(router& router) const
    {
        /* serve([&](const request& req, response& resp) { router(req, resp); }); */
        serve([&](const request& req, response& resp) { router(req, resp); });
    }

private:
    net::listener    listener;
    std::atomic_bool is_serving;

    size_t   max_header_bytes;
    uint16_t max_pending_connections;
};
}
