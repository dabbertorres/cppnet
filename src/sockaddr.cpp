#include "sockaddr.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <system_error>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"

namespace net::detail
{

socket_address to_sockaddr(const ip_addr& addr, std::uint16_t port) noexcept
{
    socket_address out;

    // NOTE: the fields are set one by one rather than designated, as their order (and some of them, e.g. BSD's sin_len)
    //       differ by platform; the rest (e.g. sin6_flowinfo and sin6_scope_id) are zeroed with the storage.
    if (addr.is_ipv4())
    {
        auto* sin       = reinterpret_cast<sockaddr_in*>(&out.storage);
        sin->sin_family = AF_INET;
        sin->sin_port   = htons(port);
        std::memcpy(&sin->sin_addr, static_cast<const std::uint8_t*>(addr), sizeof(sin->sin_addr));
        out.size = sizeof(sockaddr_in);
    }
    else
    {
        auto* sin6        = reinterpret_cast<sockaddr_in6*>(&out.storage);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port   = htons(port);
        std::memcpy(&sin6->sin6_addr, static_cast<const std::uint8_t*>(addr), sizeof(sin6->sin6_addr));
        out.size = sizeof(sockaddr_in6);
    }

    // NOTE: BSD's sin_len/sin6_len stay 0: the kernel takes the length from the call's argument instead.
    return out;
}

std::expected<address_and_port, std::error_code> from_sockaddr(const sockaddr* addr, socklen_t size) noexcept
{
    if (size < static_cast<socklen_t>(sizeof(sa_family_t)))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    switch (addr->sa_family)
    {
    case AF_INET:
    {
        if (size < static_cast<socklen_t>(sizeof(sockaddr_in)))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        sockaddr_in sin{};
        std::memcpy(&sin, addr, sizeof(sin));
        return address_and_port{.addr = ipv4_addr{ntohl(sin.sin_addr.s_addr)}, .port = ntohs(sin.sin_port)};
    }

    case AF_INET6:
    {
        if (size < static_cast<socklen_t>(sizeof(sockaddr_in6)))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        sockaddr_in6 sin6{};
        std::memcpy(&sin6, addr, sizeof(sin6));

        std::array<std::uint8_t, 16> bytes{};
        std::memcpy(bytes.data(), &sin6.sin6_addr, bytes.size());
        return address_and_port{.addr = ipv6_addr{bytes}, .port = ntohs(sin6.sin6_port)};
    }

    default: return std::unexpected(std::make_error_code(std::errc::address_family_not_supported));
    }
}

}
