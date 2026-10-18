#pragma once

#include <chrono>
#include <expected>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "socket.hpp"

namespace net
{

class tcp_socket : public socket
{
public:
    struct connect_options
    {
        protocol proto = protocol::not_care;

        // How long to wait for a connection, over all of the host's addresses. (Resolving the host isn't included:
        // getaddrinfo() has its own timeouts.)
        std::chrono::milliseconds timeout = std::chrono::seconds{10};

        bool keepalive = true;

        // Requesting stop ends connecting, failing with std::errc::operation_canceled. Resolving the host can't be
        // interrupted (getaddrinfo() blocks), so stop requested meanwhile is seen once it's done. It only covers
        // connecting: the socket returned doesn't take it (see socket::set_stop_token()).
        std::stop_token stop{};
    };

    explicit tcp_socket(io::scheduler* scheduler) noexcept;
    tcp_socket(io::scheduler* scheduler, io::handle fd);

    // connect resolves host, and connects to the first of its addresses that accepts. It fails with:
    // - an error in gai_category() if the host can't be resolved.
    // - std::errc::timed_out if no address accepted in time.
    // - std::errc::operation_canceled if the scheduler is stopped, or options.stop is.
    // - otherwise, the error from the last address tried (e.g. std::errc::connection_refused).
    static coro::task<std::expected<tcp_socket, std::error_code>>
    connect(io::scheduler* scheduler, std::string host, std::string port, connect_options options);

    static coro::task<std::expected<tcp_socket, std::error_code>>
    connect(io::scheduler* scheduler, std::string host, std::string port)
    {
        return connect(scheduler, std::move(host), std::move(port), connect_options{});
    }

    // set_nodelay turns Nagle's algorithm off (TCP_NODELAY), so a small write is sent right away, rather than when what
    // was sent before is acknowledged.
    [[nodiscard]] std::expected<void, std::error_code> set_nodelay(bool nodelay = true) const noexcept;
};

}
