#include "udp.hpp"

#include <cerrno>

#include <netdb.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

namespace net
{

udp_socket::udp_socket(std::string_view port, addr_protocol proto) : udp_socket("", port, proto) {}

udp_socket::udp_socket(std::string_view host, std::string_view port, addr_protocol proto)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
    ::addrinfo hints = {0};
#pragma clang diagnostic pop

    if (host == "") hints.ai_flags = AI_PASSIVE;

    switch (proto)
    {
    case addr_protocol::not_care:
    {
        hints.ai_family = AF_UNSPEC;
        break;
    }

    case addr_protocol::ipv4:
    {
        hints.ai_family = AF_INET;
        break;
    }

    case addr_protocol::ipv6:
    {
        hints.ai_family = AF_INET6;
        break;
    }
    }

    hints.ai_socktype = SOCK_DGRAM;

    ::addrinfo* servinfo;
    int sts = ::getaddrinfo(host != "" ? host.data() : nullptr, port.data(), &hints, &servinfo);
    if (sts != 0) throw exception{sts};

    // find first valid addr, and use it!

    for (::addrinfo* info = servinfo; info != nullptr; info = info->ai_next)
    {
        fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd > -1) break;
    }

    freeaddrinfo(servinfo);

    if (fd == invalid_fd) throw exception{"failed to bind"};
}

size_t udp_socket::read(uint8_t* data, size_t length) noexcept
{
    // size_t rcvd = 0;
    // while (rcvd < length)
    //{
    //     int num = ::recv(fd, data + rcvd, length - rcvd, 0);
    //     if (num < 0)
    //     {
    //         if (errno == EAGAIN)
    //             continue;
    //         return false;
    //     }
    //     rcvd += num;
    // }
    return 0;
}

size_t udp_socket::write(const uint8_t* data, size_t length) noexcept
{
    // size_t sent = 0;
    // while (sent < length)
    //{
    //     int num = ::sendto(fd, data + sent, length - sent, 0);
    //     if (num < 0)
    //         return false;
    //     sent += num;
    // }
    return 0;
}

}
