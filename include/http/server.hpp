#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/scheduler.hpp"

#include "listen.hpp"
#include "tcp.hpp"

namespace net::http
{

using namespace std::chrono_literals;

struct server_config
{
    std::string                     host;
    std::string                     port                    = "8080";
    std::size_t                     max_header_bytes        = 8'192;
    std::chrono::seconds            header_read_timeout     = 5s;
    std::uint16_t                   max_pending_connections = 512;
    std::shared_ptr<spdlog::logger> logger                  = spdlog::default_logger();
    bool                            http11                  = true;
    bool                            http2                   = false;
    bool                            http3                   = false;
    std::size_t                     num_threads             = std::thread::hardware_concurrency();
};

class server
{
public:
    server(io::scheduler* scheduler, router&& handler, const server_config& cfg = server_config{});

    server(const server&)            = delete;
    server& operator=(const server&) = delete;

    server(server&&) noexcept;
    server& operator=(server&&) noexcept;

    ~server();

    void         close();
    coro::task<> serve();

private:
    coro::task<>     serve_connection(tcp_socket conn) noexcept;
    void             serve_http11(tcp_socket conn) noexcept;
    void             serve_http2(tcp_socket conn) noexcept;
    std::string_view upgrade_to_protocol(const server_request& req) const noexcept;
    bool             is_protocol_supported(std::string_view protocol) const noexcept;
    bool             enforce_protocol(const server_request& req, response_writer& resp) noexcept;

    net::listener                   listener;
    std::atomic_bool                is_serving;
    router                          handler;
    std::shared_ptr<spdlog::logger> logger;
    io::scheduler*                  scheduler;

    std::size_t   max_header_bytes;
    std::uint16_t max_pending_connections;
};

}
