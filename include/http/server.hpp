#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "http/http.hpp"
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
    server(std::string_view port, const server_config& cfg);
    server(std::string_view host, std::string_view port, const server_config& cfg);
    server(const server&) = delete;
    ~server()             = default;

    void close();
    void listen(handler&) const;

private:
    net::listener    listener;
    std::atomic_bool serve;

    const size_t   max_header_bytes;
    const uint16_t max_pending_connections;
};

}
