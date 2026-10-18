#include "tcp.hpp"

#include <cerrno>
#include <chrono>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <system_error>

#include <netdb.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include "coro/task.hpp"
#include "exception.hpp"
#include "io/poll.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "socket.hpp"

namespace
{

using addrinfo_ptr = std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)>;

std::expected<addrinfo_ptr, std::error_code>
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

    if (err != 0) return std::unexpected(net::make_gai_error_code(err));

    return addrinfo_ptr{found, &::freeaddrinfo};
}

std::error_code from_errno(int err) noexcept { return {err, std::system_category()}; }

// wait_connected waits for a connection that's in progress to be established, returning the error if it fails.
//
// The connection is established (or failed) once the socket is writable. The socket may be reported ready before then
// (e.g. as Linux reports a socket that isn't connected yet as hung up, when it's registered), so that's checked, and it
// waits again if it's still connecting.
net::coro::task<std::error_code> wait_connected(net::io::scheduler*        scheduler,
                                                net::io::handle            fd,
                                                net::io::clock::time_point deadline,
                                                std::stop_token            stop)
{
    while (true)
    {
        if (auto err = co_await scheduler->wait_until(fd, net::io::poll_op::write, deadline, stop); err) co_return err;

        int       sock_err = 0;
        socklen_t size     = sizeof(sock_err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &size) == -1) sock_err = errno;
        if (sock_err != 0) co_return from_errno(sock_err);

        sockaddr_storage peer{};
        socklen_t        peer_size = sizeof(peer);
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_size) == 0) co_return std::error_code{};
        if (errno != ENOTCONN) co_return from_errno(errno);
    }
}
}

namespace net
{

tcp_socket::tcp_socket(io::scheduler* scheduler) noexcept
    : socket{scheduler, invalid_fd}
{}

tcp_socket::tcp_socket(io::scheduler* scheduler, io::handle fd)
    : socket{scheduler, fd}
{}

std::expected<void, std::error_code> tcp_socket::set_nodelay(bool nodelay) const noexcept
{
    const int value = nodelay ? 1 : 0;
    if (::setsockopt(native_handle(), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) == -1)
        return std::unexpected(std::error_code{errno, std::system_category()});
    return {};
}

coro::task<std::expected<tcp_socket, std::error_code>>
tcp_socket::connect(io::scheduler* scheduler, std::string host, std::string port, connect_options options)
{
    // getaddrinfo() blocks, so keep it off the worker threads.
    auto resolved =
        co_await scheduler->run_blocking([host, port, proto = options.proto] { return resolve(host, port, proto); },
                                         options.stop);
    if (!resolved.has_value()) co_return std::unexpected(resolved.error());

    // resolving can't be interrupted, so its result is dropped if stop was requested meanwhile.
    if (options.stop.stop_requested()) co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));

    auto& addresses = *resolved;
    if (!addresses.has_value()) co_return std::unexpected(addresses.error());

    const auto deadline = io::clock::now() + options.timeout;

    std::error_code last_err = std::make_error_code(std::errc::host_unreachable);

    for (const auto* info = addresses->get(); info != nullptr; info = info->ai_next)
    {
        io::handle fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
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

        auto result = co_await wait_connected(scheduler, fd, deadline, options.stop);
        if (!result) co_return conn;

        conn.close(false);

        // a timeout is for all of the addresses together, so there's no time left to try the rest.
        if (result == std::errc::timed_out || result == std::errc::operation_canceled)
            co_return std::unexpected(result);

        last_err = result;
    }

    co_return std::unexpected(last_err);
}

}
