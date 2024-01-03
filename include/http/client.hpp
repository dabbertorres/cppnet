#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <shared_mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "http/request.hpp"
#include "http/response.hpp"
#include "util/resource_pool.hpp"

#include "ip_addr.hpp"
#include "tcp.hpp"

namespace net::http
{

using namespace std::chrono_literals;

class client
{
public:
    client(std::size_t max_connections_per_host = 2, std::chrono::microseconds timeout = 15us, bool keepalives = true);

    client(const client&)            = delete;
    client& operator=(const client&) = delete;

    client(client&&) noexcept;
    client& operator=(client&&) noexcept;

    ~client() = default;

    std::expected<client_response, std::error_condition> send(const client_request& request) noexcept;

private:
    using host_connections = util::resource_pool<tcp_socket>;

    host_connections::borrowed_resource get_connection(const std::string& host) noexcept;

    using connections_ptr = std::unique_ptr<host_connections>;
    using connection_pool = std::unordered_map<std::string, connections_ptr>;

    connection_pool           connections;
    std::shared_mutex         connections_mu;
    std::size_t               max_connections_per_host;
    net::protocol             protocol = net::protocol::not_care;
    bool                      keepalives;
    std::chrono::microseconds timeout;
};

}
