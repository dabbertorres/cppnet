#pragma once

#include <cerrno>
#include <cstdint>
#include <expected>
#include <system_error>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

#include "ipv4_addr.hpp"
#include "sockaddr.hpp"

// A port on loopback that refuses TCP connections, for the tests of what's refused.
namespace test
{

// held_port is a port on 127.0.0.1 that refuses TCP connections for as long as it lives (port()).
//
// A port let go (bound, then closed) isn't enough: the OS may give it to another socket before the test connects there,
// e.g. a listener (the test's own, another test's, another process's), or the very socket that connects there, which
// then connects to itself (a simultaneous open) rather than being refused. So it's held, by a connection: by a socket
// bound to it and connected to a listener of the holder's own. A connection to the port from any other socket matches
// neither a listener nor a connection, so the OS refuses it (with a reset). The socket is bound before connecting, as
// on Linux a port that connect() chose itself can be shared with other connections (to other peers), even one made to
// it from it. And not just bound and left without listening: macOS drops a connection's SYN to such a port, without a
// reset, so connecting would hang until its timeout rather than be refused.
//
// NOTE: POSIX sockets, blocking (each call finishes at once, on loopback), as cppnet's connect can't bind first.
class held_port
{
public:
    // make holds a port, or fails with why it couldn't.
    [[nodiscard]] static std::expected<held_port, std::error_code> make()
    {
        held_port held;

        // a listener of its own, on a port the OS chooses.
        held.listening = ::socket(AF_INET, SOCK_STREAM, 0);
        if (held.listening == -1) return std::unexpected(last_error());

        const auto any = net::detail::to_sockaddr(net::ipv4_addr::loopback(), 0);
        if (::bind(held.listening, any.get(), any.size) != 0) return std::unexpected(last_error());
        if (::listen(held.listening, 1) != 0) return std::unexpected(last_error());

        auto listening_port = local_port(held.listening);
        if (!listening_port.has_value()) return std::unexpected(listening_port.error());

        // the port held: bound, then connected to the listener.
        held.holding = ::socket(AF_INET, SOCK_STREAM, 0);
        if (held.holding == -1) return std::unexpected(last_error());
        if (::bind(held.holding, any.get(), any.size) != 0) return std::unexpected(last_error());

        const auto to = net::detail::to_sockaddr(net::ipv4_addr::loopback(), *listening_port);
        if (::connect(held.holding, to.get(), to.size) != 0) return std::unexpected(last_error());

        held.accepted = ::accept(held.listening, nullptr, nullptr);
        if (held.accepted == -1) return std::unexpected(last_error());

        auto port = local_port(held.holding);
        if (!port.has_value()) return std::unexpected(port.error());
        held.held = *port;

        return held;
    }

    held_port(const held_port&)            = delete;
    held_port& operator=(const held_port&) = delete;

    held_port(held_port&& other) noexcept
        : listening{std::exchange(other.listening, -1)}
        , holding{std::exchange(other.holding, -1)}
        , accepted{std::exchange(other.accepted, -1)}
        , held{other.held}
    {}

    held_port& operator=(held_port&&) = delete;

    // NOTE: the connection's ends are reset (closed with a zero linger), so neither is left in TIME_WAIT, which would
    //       keep its port from being bound for a minute or so.
    ~held_port() noexcept
    {
        const linger reset{.l_onoff = 1, .l_linger = 0};
        for (const int fd : {accepted, holding})
        {
            if (fd != -1) ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        }

        for (const int fd : {accepted, holding, listening})
        {
            if (fd != -1) ::close(fd);
        }
    }

    // port is the port held, on 127.0.0.1.
    [[nodiscard]] std::uint16_t port() const noexcept { return held; }

private:
    held_port() noexcept = default;

    static std::error_code last_error() noexcept { return {errno, std::system_category()}; }

    static std::expected<std::uint16_t, std::error_code> local_port(int fd) noexcept
    {
        net::detail::socket_address addr;
        addr.size = sizeof(addr.storage);
        if (::getsockname(fd, addr.get(), &addr.size) != 0) return std::unexpected(last_error());

        auto parsed = net::detail::from_sockaddr(addr);
        if (!parsed.has_value()) return std::unexpected(parsed.error());
        return parsed->port;
    }

    int           listening = -1;
    int           holding   = -1;
    int           accepted  = -1;
    std::uint16_t held      = 0;
};

}
