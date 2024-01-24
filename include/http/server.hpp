#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include "coro/thread_pool.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"

#include "listen.hpp"
#include "tcp.hpp"

namespace net::http
{

using namespace std::chrono_literals;

struct server_config
{
    std::string                     host                    = "localhost";
    std::string                     port                    = "8080";
    std::size_t                     max_header_bytes        = 8'192;
    std::chrono::seconds            header_read_timeout     = 5s; // NOLINT(missing-includes)
    std::uint16_t                   max_pending_connections = 512;
    std::shared_ptr<spdlog::logger> logger                  = spdlog::default_logger();
    bool                            http11                  = true;
    bool                            http2                   = false;
    bool                            http3                   = false;
    std::size_t                     num_threads             = std::thread::hardware_concurrency();
    // TODO: coroutines
};

class server
{
public:
    server(router&& handler, const server_config& cfg = server_config{});

    server(const server&)            = delete;
    server& operator=(const server&) = delete;

    server(server&&) noexcept;
    server& operator=(server&&) noexcept;

    ~server();

    void close();
    void serve();

private:
    void             serve_connection(tcp_socket&& client_sock) noexcept;
    std::string_view upgrade_to_protocol(const server_request& req) const noexcept;
    bool             is_protocol_supported(std::string_view protocol) const noexcept;
    bool             enforce_protocol(const server_request& req, response_writer& resp) noexcept;

    net::listener                   listener;
    std::atomic_bool                is_serving;
    router                          handler;
    std::shared_ptr<spdlog::logger> logger;
    std::vector<std::thread>        connections;
    std::mutex                      connections_mu;
    coro::thread_pool               threads;

    std::size_t   max_header_bytes;
    std::uint16_t max_pending_connections;
};

}
