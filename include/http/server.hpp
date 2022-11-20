#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "util/thread_pool.hpp"

#include "listen.hpp"

namespace net::http
{

struct server_config
{
    std::string                     host = "localhost";
    std::string                     port = "8080";
    router                          router;
    std::size_t                     max_header_bytes        = 8192;
    std::chrono::seconds            header_read_timeout     = 5s;
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
    server(const server_config& cfg = server_config{});

    server(const server&)            = delete;
    server& operator=(const server&) = delete;

    server(server&&) noexcept;
    server& operator=(server&&) noexcept;

    ~server();

    void close();
    void serve();

private:
    bool enforce_protocol(const server_request& req, response_writer& resp) noexcept;

    net::listener                   listener;
    std::atomic_bool                is_serving;
    router                          router;
    std::shared_ptr<spdlog::logger> logger;
    std::vector<std::thread>        connections;
    std::mutex                      connections_mu;
    util::thread_pool               threads;

    size_t   max_header_bytes;
    uint16_t max_pending_connections;
};

}
