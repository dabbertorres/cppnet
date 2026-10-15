#include "tcp.hpp"

#include <cerrno>
#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "coro/task.hpp"
#include "exception.hpp"
#include "io/poll.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "socket.hpp"

namespace
{

using addrinfo_ptr = std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)>;

std::expected<addrinfo_ptr, std::error_condition>
resolve(const std::string& host, const std::string& port, net::protocol proto) noexcept
{
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;

    switch (proto)
    {
    case net::protocol::not_care: hints.ai_family = AF_UNSPEC; break;
    case net::protocol::ipv4: hints.ai_family = AF_INET; break;
    case net::protocol::ipv6: hints.ai_family = AF_INET6; break;
    }

    // An IP address is used as is. A name only resolves to the kinds of address this host has (AI_ADDRCONFIG), but that
    // would also reject an IPv6 address (even "::1") when there's no IPv6 network configured: glibc doesn't count
    // loopback addresses.
    addrinfo* found = nullptr;

    hints.ai_flags = AI_NUMERICHOST;
    auto err       = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &found);
    if (err == EAI_NONAME)
    {
        hints.ai_flags = AI_ADDRCONFIG;
        err            = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &found);
    }

    if (err != 0) return std::unexpected(net::make_gai_error_condition(err));

    return addrinfo_ptr{found, &::freeaddrinfo};
}

std::error_condition from_errno(int err) noexcept { return {err, std::generic_category()}; }

}

namespace net
{

tcp_socket::tcp_socket(io::scheduler* scheduler) noexcept
    : socket{scheduler, invalid_fd}
{}

tcp_socket::tcp_socket(io::scheduler* scheduler, int fd)
    : socket{scheduler, fd}
{}

coro::task<std::expected<tcp_socket, std::error_condition>>
tcp_socket::connect(io::scheduler* scheduler, std::string host, std::string port, connect_options options)
{
    // getaddrinfo() blocks, so keep it off the worker threads.
    auto addresses = co_await scheduler->run_blocking([host, port, proto = options.proto]
                                                      { return resolve(host, port, proto); });
    if (!addresses.has_value()) co_return std::unexpected(addresses.error());

    const auto deadline = io::clock::now() + options.timeout;

    std::error_condition last_err = std::make_error_condition(std::errc::host_unreachable);

    for (auto* info = addresses->get(); info != nullptr; info = info->ai_next)
    {
        int fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd == -1)
        {
            last_err = from_errno(errno);
            continue;
        }

        // makes the socket non-blocking before connecting, and owns it from here on.
        tcp_socket conn{scheduler, fd};

        if (options.keepalive)
        {
            int yes = 1;
            set_option(fd, SO_KEEPALIVE, &yes);
        }

        if (::connect(fd, info->ai_addr, info->ai_addrlen) == 0) co_return conn;

        if (errno != EINPROGRESS)
        {
            last_err = from_errno(errno);
            conn.close(false);
            continue;
        }

        // the connection is established (or failed) once the socket is writable.
        auto wait_err = co_await scheduler->wait_until(fd, io::poll_op::write, deadline);
        if (wait_err)
        {
            conn.close(false);

            // a timeout is for all of the addresses together, so there's no time left to try the rest.
            if (wait_err == std::errc::timed_out || wait_err == std::errc::operation_canceled)
                co_return std::unexpected(wait_err);

            last_err = wait_err;
            continue;
        }

        int       sock_err = 0;
        socklen_t size     = sizeof(sock_err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &size) == -1) sock_err = errno;
        if (sock_err == 0) co_return conn;

        last_err = from_errno(sock_err);
        conn.close(false);
    }

    co_return std::unexpected(last_err);
}

}
