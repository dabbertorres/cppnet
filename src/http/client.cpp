#include "http/client.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <utility>

#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/http2.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/scheduler.hpp"

#include "tcp.hpp"

namespace net::http
{

client::client(io::scheduler*            scheduler,
               std::size_t               max_connections_per_host,
               std::chrono::microseconds timeout,
               bool                      keepalives)
    : scheduler{scheduler}
    , max_connections_per_host{max_connections_per_host}
    , keepalives{keepalives}
    , timeout{timeout}
{}

client::client(client&& other) noexcept
    : scheduler{std::exchange(other.scheduler, nullptr)}
    , max_connections_per_host{std::exchange(other.max_connections_per_host, 0)}
    , keepalives{std::exchange(other.keepalives, false)}
    , timeout{std::exchange(other.timeout, 0us)}
{
    std::unique_lock lock{other.connections_mu};
    connections = std::exchange(other.connections, connection_pool{});
}

client& client::operator=(client&& other) noexcept
{
    std::scoped_lock lock{connections_mu, other.connections_mu};

    max_connections_per_host = std::exchange(other.max_connections_per_host, 0);
    connections              = std::exchange(other.connections, connection_pool{});

    return *this;
}

std::expected<client_response, std::error_condition> client::send(const client_request& request) noexcept
{
    request_encoder  encode = nullptr;
    response_decoder decode = nullptr;

    if (request.version == protocol_version{0, 0} || request.version == protocol_version{1, 1})
    {
        encode = &http11::request_encode;
        decode = &http11::response_decode;
    }
    else if (request.version == protocol_version{2, 0})
    {
        encode = &http2::request_encode;
        decode = &http2::response_decode;
    }
    else
    {
        // TODO: return unsupported protocol
        return {};
    }

    auto conn = get_connection(request.uri.host, request.uri.port);
    auto res  = encode(conn.get(), request);
    if (res.has_error()) return std::unexpected(res.to_error());

    // TODO: NO MORE MEMORY LEAK
    // Need to change response_decoder (and friends) to take a unique_ptr/shared_ptr
    // as an argument instead of a raw pointer whenever the reader is returned to the
    // caller.
    auto* reader = new io::buffered_reader(conn.get());
    auto  resp   = decode(reader, std::numeric_limits<std::size_t>::max());
    if (resp.has_error()) return std::unexpected(resp.to_error());

    return resp.to_value();
}

client::host_connections::borrowed_resource client::get_connection(const std::string& host,
                                                                   const std::string& port) noexcept
{
    auto connection_host = host;
    if (!port.empty())
    {
        connection_host += ":" + port;
    }

    std::shared_lock read_lock{connections_mu};

    auto it = connections.find(connection_host);
    if (it == connections.end())
    {
        read_lock.unlock();

        {
            std::lock_guard write_lock{connections_mu};

            // check again if someone else added it before us...
            it = connections.find(connection_host);
            if (it == connections.end())
            {
                connections[connection_host] = std::make_unique<host_connections>(
                    max_connections_per_host,
                    max_connections_per_host,
                    [&, this] { return std::make_unique<tcp_socket>(scheduler, host, port); });
            }
        }

        // restore the lock
        read_lock.lock();
    }

    it = connections.find(connection_host);
    return it->second->get();
}

}
