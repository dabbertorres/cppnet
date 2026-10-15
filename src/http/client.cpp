#include "http/client.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/scheduler.hpp"
#include "tcp.hpp"

namespace
{

// a server's response head can't use up all of the client's memory.
constexpr std::size_t max_response_header_bytes = 1'024 * 1'024;

// connection_reader reads a response from the connection it owns, so the connection lives as long as the response.
//
// NOTE: the socket is held in a base class, so that it's constructed before (and destroyed after) the buffered_reader
//       that reads from it.
struct socket_holder
{
    std::unique_ptr<net::tcp_socket> socket;
};

class connection_reader final
    : private socket_holder
    , public net::io::buffered_reader
{
public:
    explicit connection_reader(std::unique_ptr<net::tcp_socket> conn)
        : socket_holder{std::move(conn)}
        , net::io::buffered_reader{socket.get()}
    {}
};

}

namespace net::http
{

client::client(io::scheduler* scheduler, std::chrono::milliseconds connect_timeout, bool keepalives)
    : scheduler{scheduler}
    , connect_timeout{connect_timeout}
    , keepalives{keepalives}
{}

coro::task<std::expected<client_response, std::error_condition>> client::send(const client_request& request)
{
    // Only HTTP/1.x is implemented. In particular, the http2 encoders are stubs.
    if (request.version.major > 1) co_return std::unexpected(std::make_error_condition(std::errc::not_supported));

    auto conn = co_await tcp_socket::connect(scheduler,
                                             request.uri.host,
                                             !request.uri.port.empty() ? request.uri.port : "80",
                                             {.timeout = connect_timeout, .keepalive = keepalives});
    if (!conn.has_value()) co_return std::unexpected(conn.error());

    auto sock = std::make_unique<tcp_socket>(std::move(*conn));

    auto res = co_await http11::request_encode(sock.get(), request);
    if (!res.has_value()) co_return std::unexpected(res.error());

    auto resp = co_await http11::response_decode(std::make_unique<connection_reader>(std::move(sock)),
                                                 request.method,
                                                 max_response_header_bytes);
    if (!resp.has_value()) co_return std::unexpected(resp.error());

    co_return std::move(*resp);
}

}
