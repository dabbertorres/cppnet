#include "http/server.hpp"

#include <string>

namespace net::http
{

server::server(const std::string& port, const server_config& cfg)
    : listener(port,
               network::tcp,
               protocol::not_care,
               std::chrono::duration_cast<std::chrono::microseconds>(cfg.header_read_timeout))
    , is_serving{false}
    , max_header_bytes{cfg.max_header_bytes}
    , max_pending_connections{cfg.max_pending_connections}
{}

server::server(const std::string& host, const std::string& port, const server_config& cfg)
    : listener(host, port, network::tcp, protocol::not_care, cfg.header_read_timeout)
    , is_serving{false}
    , max_header_bytes{cfg.max_header_bytes}
    , max_pending_connections{cfg.max_pending_connections}
{}

server::server(server&& other) noexcept
    : listener{std::move(other.listener)}
    , is_serving{other.is_serving.exchange(false)}
    , max_header_bytes{other.max_header_bytes}
    , max_pending_connections{other.max_pending_connections}
{}

server& server::operator=(server&& other) noexcept
{
    if (is_serving)
    {
        close();
        listener.~listener();
    }

    is_serving              = other.is_serving.exchange(false);
    max_header_bytes        = other.max_header_bytes;
    max_pending_connections = other.max_pending_connections;

    return *this;
}

server::~server() { close(); }

void server::close()
{
    is_serving = false;
    is_serving.notify_all();
}

}
