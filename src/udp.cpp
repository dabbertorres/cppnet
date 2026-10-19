#include "udp.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <system_error>

#include <sys/socket.h>

#include "coro/task.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "sockaddr.hpp"
#include "socket.hpp"

namespace
{

std::error_code last_error() noexcept { return {errno, std::system_category()}; }

// open makes a socket of family's, owned (and registered with the scheduler) from the start.
std::expected<net::udp_socket, std::error_code> open(net::io::scheduler* scheduler, int family) noexcept
{
    const net::io::handle fd = ::socket(family, SOCK_DGRAM, 0);
    if (fd == -1) return std::unexpected(last_error());

    try
    {
        // NOTE: closes fd if it throws.
        return net::udp_socket{scheduler, fd};
    }
    catch (const std::system_error& ex)
    {
        return std::unexpected(ex.code());
    }
}

}

namespace net
{

udp_socket::udp_socket(io::scheduler* scheduler, io::handle fd)
    : socket{scheduler, fd}
{}

std::expected<udp_socket, std::error_code>
udp_socket::connect(io::scheduler* scheduler, const ip_addr& addr, std::uint16_t port) noexcept
{
    const auto to = detail::to_sockaddr(addr, port);

    auto sock = open(scheduler, to.family());
    if (!sock.has_value()) return sock;

    // NOTE: a datagram socket connects at once: nothing is sent.
    if (::connect(sock->native_handle(), to.get(), to.size) != 0) return std::unexpected(last_error());
    return sock;
}

std::expected<udp_socket, std::error_code>
udp_socket::bind(io::scheduler* scheduler, const ip_addr& addr, std::uint16_t port) noexcept
{
    const auto at = detail::to_sockaddr(addr, port);

    auto sock = open(scheduler, at.family());
    if (!sock.has_value()) return sock;

    // NOTE: no SO_REUSEADDR (unlike a TCP listener's): for UDP, it would let another socket bind the same port, and
    //       take its datagrams.
    if (::bind(sock->native_handle(), at.get(), at.size) != 0) return std::unexpected(last_error());
    return sock;
}

coro::task<std::expected<udp_socket::datagram, std::error_code>> udp_socket::receive_from(std::span<std::byte> data)
{
    detail::socket_address from;

    auto res = co_await read_from(data, from);
    if (res.err) co_return std::unexpected(res.err);

    auto sender = detail::from_sockaddr(from);
    if (!sender.has_value()) co_return std::unexpected(sender.error());

    co_return datagram{.size = res.count, .addr = sender->addr, .port = sender->port};
}

coro::task<std::expected<std::size_t, std::error_code>>
udp_socket::send_to(std::span<const std::byte> data, ip_addr addr, std::uint16_t port)
{
    const auto to = detail::to_sockaddr(addr, port);

    auto res = co_await write_to(data, to);
    if (res.err) co_return std::unexpected(res.err);
    co_return res.count;
}

}
