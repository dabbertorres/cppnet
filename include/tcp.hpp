#pragma once

#include <chrono>
#include <expected>
#include <string>
#include <system_error>

#include "coro/task.hpp"
#include "io/scheduler.hpp"

#include "ip_addr.hpp"
#include "socket.hpp"

namespace net
{

using namespace std::chrono_literals;

class tcp_socket : public socket
{
public:
    struct connect_options
    {
        protocol proto = protocol::not_care;

        // How long to wait for a connection, over all of the host's addresses. (Resolving the host isn't included:
        // getaddrinfo() has its own timeouts.)
        std::chrono::milliseconds timeout = 10s;

        bool keepalive = true;
    };

    tcp_socket(io::scheduler* scheduler) noexcept;
    tcp_socket(io::scheduler* scheduler, int fd);

    // connect resolves host, and connects to the first of its addresses that accepts. It fails with:
    // - an error in gai_category() if the host can't be resolved.
    // - std::errc::timed_out if no address accepted in time.
    // - std::errc::operation_canceled if the scheduler is stopped.
    // - otherwise, the error from the last address tried (e.g. std::errc::connection_refused).
    static coro::task<std::expected<tcp_socket, std::error_condition>>
    connect(io::scheduler* scheduler, std::string host, std::string port, connect_options options);

    static coro::task<std::expected<tcp_socket, std::error_condition>>
    connect(io::scheduler* scheduler, std::string host, std::string port)
    {
        return connect(scheduler, std::move(host), std::move(port), connect_options{});
    }
};

}
