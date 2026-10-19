#include "tcp.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include "coro/task.hpp"
#include "exception.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "sockaddr.hpp"
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

    // An IP address is used as is (e.g. one with an IPv6 zone, or in a form literal() leaves to getaddrinfo()). A name
    // only resolves to the kinds of address this host has (AI_ADDRCONFIG), but that would also reject an IPv6 address
    // (even "::1") when there's no IPv6 network configured: glibc doesn't count loopback addresses.
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

// literal is host's address and port, if host is an IP address and port a number, so connecting needn't resolve them.
//
// An IPv4 address must be in its usual form (as to_string() gives it): getaddrinfo() also takes others, which it reads
// differently (e.g. "010.0.0.1" as octal, 8.0.0.1, where ip_addr::parse() reads 10.0.0.1, and "127.1" as 127.0.0.1),
// so they're left to it. So are IPv6 addresses with a zone ("fe80::1%en0"), which ip_addr doesn't have.
std::optional<net::detail::socket_address> literal(const std::string& host, const std::string& port) noexcept
{
    std::uint16_t number = 0;

    const auto* end    = port.data() + port.size();
    auto [ptr, failed] = std::from_chars(port.data(), end, number);
    if (port.empty() || failed != std::errc{} || ptr != end) return std::nullopt;

    auto addr = net::ip_addr::parse(host);
    if (!addr.has_value()) return std::nullopt;

    try
    {
        if (addr->is_ipv4() && addr->to_string() != host) return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt; // to_string() can only fail to allocate: getaddrinfo() decides, as it did before.
    }

    return net::detail::to_sockaddr(*addr, number);
}

bool of_protocol(const net::detail::socket_address& addr, net::protocol proto) noexcept
{
    switch (proto)
    {
    case net::protocol::not_care: return addr.family() == AF_INET || addr.family() == AF_INET6;
    case net::protocol::ipv4: return addr.family() == AF_INET;
    case net::protocol::ipv6: return addr.family() == AF_INET6;
    }

    return false;
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

// connect_to connects to the first of addresses (those of options.proto's family) that accepts, in order, each attempt
// getting its share of options.timeout (see net::detail::attempt_deadline()).
net::coro::task<std::expected<net::tcp_socket, std::error_code>>
connect_to(net::io::scheduler*                      scheduler,
           std::vector<net::detail::socket_address> addresses,
           net::tcp_socket::connect_options         options)
{
    const auto given = addresses.size();
    std::erase_if(addresses, [&](const auto& addr) { return !of_protocol(addr, options.proto); });
    if (addresses.empty())
    {
        co_return std::unexpected(
            std::make_error_code(given == 0 ? std::errc::invalid_argument : std::errc::address_family_not_supported));
    }

    const auto deadline = net::io::clock::now() + options.timeout;

    std::error_code last_err;

    for (std::size_t i = 0; i < addresses.size(); ++i)
    {
        if (options.stop.stop_requested())
            co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));

        // an attempt that timed out took its share, and the time may be up for the rest too.
        const auto now = net::io::clock::now();
        if (now >= deadline) co_return std::unexpected(std::make_error_code(std::errc::timed_out));

        const auto&     addr = addresses[i];
        net::io::handle fd   = ::socket(addr.family(), SOCK_STREAM, 0);
        if (fd == -1)
        {
            last_err = from_errno(errno);
            continue;
        }

        // makes the socket non-blocking before connecting, and owns it from here on.
        net::tcp_socket conn{scheduler, fd};

        if (options.keepalive)
        {
            const int yes = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
        }

        if (::connect(fd, addr.get(), addr.size) == 0) co_return conn;

        if (errno != EINPROGRESS)
        {
            last_err = from_errno(errno);
            conn.close(false);
            continue;
        }

        const auto attempt = net::detail::attempt_deadline(now, deadline, addresses.size() - i);

        auto result = co_await wait_connected(scheduler, fd, attempt, options.stop);
        if (!result) co_return conn;

        conn.close(false);

        // a timeout only ends this attempt, its share of the time being up: the rest go on with what's left.
        if (result == std::errc::operation_canceled) co_return std::unexpected(result);

        last_err = result;
    }

    co_return std::unexpected(last_err);
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

io::clock::time_point
detail::attempt_deadline(io::clock::time_point now, io::clock::time_point deadline, std::size_t remaining) noexcept
{
    // an equal share for each address left, unless that's too short to connect: then this one takes more (up to all of
    // what's left), at the expense of the last ones.
    constexpr io::clock::duration sane_minimum = std::chrono::seconds{2};

    const auto left  = deadline - now;
    auto       share = left / static_cast<io::clock::rep>(std::max<std::size_t>(remaining, 1));
    if (share < sane_minimum) share = std::min(left, sane_minimum);

    return now + share;
}

coro::task<std::expected<tcp_socket, std::error_code>>
tcp_socket::connect(io::scheduler* scheduler, std::string host, std::string port, connect_options options)
{
    if (options.stop.stop_requested()) co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));

    std::vector<detail::socket_address> addresses;

    if (auto addr = literal(host, port); addr.has_value())
    {
        addresses.push_back(*addr);
    }
    else
    {
        // getaddrinfo() blocks, so keep it off the worker threads.
        auto resolved =
            co_await scheduler->run_blocking([host, port, proto = options.proto] { return resolve(host, port, proto); },
                                             options.stop);
        if (!resolved.has_value()) co_return std::unexpected(resolved.error());

        // resolving can't be interrupted, so its result is dropped if stop was requested meanwhile.
        if (options.stop.stop_requested())
            co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));

        auto& found = *resolved;
        if (!found.has_value()) co_return std::unexpected(found.error());

        // NOTE: the addresses as getaddrinfo() gave them, so an IPv6 address keeps its zone.
        for (const auto* info = found->get(); info != nullptr; info = info->ai_next)
        {
            if (info->ai_addrlen > sizeof(sockaddr_storage)) continue;

            auto& addr = addresses.emplace_back();
            std::memcpy(&addr.storage, info->ai_addr, info->ai_addrlen);
            addr.size = info->ai_addrlen;
        }
    }

    co_return co_await connect_to(scheduler, std::move(addresses), std::move(options));
}

coro::task<std::expected<tcp_socket, std::error_code>> tcp_socket::connect(io::scheduler*           scheduler,
                                                                           std::span<const ip_addr> addresses,
                                                                           std::uint16_t            port,
                                                                           const connect_options&   options)
{
    // NOTE: not a coroutine, so the addresses are converted (and so copied) before it returns.
    std::vector<detail::socket_address> converted;
    converted.reserve(addresses.size());
    for (const auto& addr : addresses) converted.push_back(detail::to_sockaddr(addr, port));

    return connect_to(scheduler, std::move(converted), options);
}

}
