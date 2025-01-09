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

#include "coro/task.hpp"
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

coro::task<std::expected<client_response, std::error_condition>> client::send(const client_request& request)
{
    request_encoder  encode = nullptr;
    response_decoder decode = nullptr;

    if (request.version.major <= 1)
    {
        encode = &http11::request_encode;
        decode = &http11::response_decode;
    }
    else if (request.version == protocol_version{.major = 2, .minor = 0})
    {
        encode = &http2::request_encode;
        decode = &http2::response_decode;
    }
    else
    {
        // TODO: return unsupported protocol
        co_return std::unexpected(std::make_error_condition(std::errc::not_supported));
    }

    auto conn = get_connection(request.uri.host, request.uri.port);
    auto res  = co_await encode(conn.get(), request);
    if (!res.has_value()) co_return std::unexpected(res.error());

    auto reader = std::make_unique<io::buffered_reader>(conn.get());
    auto resp   = co_await decode(std::move(reader), std::numeric_limits<std::size_t>::max());
    if (!resp.has_value()) co_return std::unexpected(resp.error());

    co_return {std::move(resp.value())};
}

client::host_connections::borrowed_resource client::get_connection(const std::string& host, const std::string& port)
{
    auto connection_host = host + ":" + (!port.empty() ? port : "80");

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
                    [&, this] { return std::make_unique<tcp_socket>(scheduler, host, (!port.empty() ? port : "80")); });
            }
        }

        // restore the lock
        read_lock.lock();
    }

    it = connections.find(connection_host);
    return it->second->get();
}

}
