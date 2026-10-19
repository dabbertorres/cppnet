#pragma once

#include <cstdint>
#include <expected>
#include <system_error>

#include <sys/socket.h>

#include "ip_addr.hpp"

// Internal: converting between ip_addr and the sockets API's addresses, for the sockets.

namespace net::detail
{

// socket_address is an address as the sockets API takes and gives one (connect(), bind(), sendto(), recvfrom(), ...):
// a sockaddr_in or a sockaddr_in6 (or, from getaddrinfo(), an IPv6 address with its zone), and its length.
struct socket_address
{
    sockaddr_storage storage{};
    socklen_t        size = 0;

    [[nodiscard]] const sockaddr* get() const noexcept { return reinterpret_cast<const sockaddr*>(&storage); }
    [[nodiscard]] sockaddr*       get() noexcept { return reinterpret_cast<sockaddr*>(&storage); }

    [[nodiscard]] int family() const noexcept { return storage.ss_family; }
};

// to_sockaddr is addr and port (in host byte order) as a sockaddr_in or sockaddr_in6. (ip_addr has no IPv6 zone, so its
// scope id is 0.)
[[nodiscard]] socket_address to_sockaddr(const ip_addr& addr, std::uint16_t port) noexcept;

// address_and_port is what a socket address holds, the port in host byte order.
struct address_and_port
{
    ip_addr       addr;
    std::uint16_t port = 0;
};

// from_sockaddr is the address and port of a sockaddr_in or sockaddr_in6 of size bytes (an IPv6 address's zone is
// dropped). It fails with std::errc::address_family_not_supported for any other family, or std::errc::invalid_argument
// if size is too small for its family's.
[[nodiscard]] std::expected<address_and_port, std::error_code> from_sockaddr(const sockaddr* addr,
                                                                             socklen_t       size) noexcept;

[[nodiscard]] inline std::expected<address_and_port, std::error_code> from_sockaddr(const socket_address& addr) noexcept
{
    return from_sockaddr(addr.get(), addr.size);
}

}
