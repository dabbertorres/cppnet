#include "udp.hpp"

#include <cerrno>

#include <netdb.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

namespace net
{

udp_socket::udp_socket(std::string_view port, protocol proto) : udp_socket("", port, proto) {}

udp_socket::udp_socket(std::string_view host, std::string_view port, protocol proto)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
    ::addrinfo hints = {0};
#pragma clang diagnostic pop

    if (host == "") hints.ai_flags = AI_PASSIVE;

    switch (proto)
    {
    case protocol::not_care: hints.ai_family = AF_UNSPEC; break;
    case protocol::ipv4: hints.ai_family = AF_INET; break;
    case protocol::ipv6: hints.ai_family = AF_INET6; break;
    }

    hints.ai_socktype = SOCK_DGRAM;

    ::addrinfo* servinfo;
    int sts = ::getaddrinfo(host != "" ? host.data() : nullptr, port.data(), &hints, &servinfo);
    if (sts != 0) throw_for_gai_error(sts);

    // find first valid addr, and use it!

    for (::addrinfo* info = servinfo; info != nullptr; info = info->ai_next)
    {
        fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd > -1) break;
    }

    freeaddrinfo(servinfo);

    if (fd == invalid_fd) throw exception{"failed to bind"};
}

io_result udp_socket::read(uint8_t* data, size_t length) noexcept
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
    return {.count = 0, .err = {}};
}

io_result udp_socket::write(const uint8_t* data, size_t length) noexcept
{
    // size_t sent = 0;
    // while (sent < length)
    //{
    //     int num = ::sendto(fd, data + sent, length - sent, 0);
    //     if (num < 0)
    //         return false;
    //     sent += num;
    // }
    return {.count = 0, .err = {}};
}

}
