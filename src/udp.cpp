#include "udp.hpp"

#include <string_view>

#include <netdb.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#include "io/scheduler.hpp"

#include "exception.hpp"
#include "ip_addr.hpp"
#include "socket.hpp"

namespace net
{

udp_socket::udp_socket(io::scheduler* scheduler, int fd) noexcept
    : socket{scheduler, fd}
{}

udp_socket::udp_socket(io::scheduler* scheduler, std::string_view port, protocol proto)
    : udp_socket{scheduler, "", port, proto}
{}

udp_socket::udp_socket(io::scheduler* scheduler, std::string_view host, std::string_view port, protocol proto)
    : socket{scheduler, open(host, port, proto)}
{}

int udp_socket::open(std::string_view host, std::string_view port, protocol proto)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
    ::addrinfo hints = {0};
#pragma clang diagnostic pop

    if (host.empty()) hints.ai_flags = AI_PASSIVE;

    switch (proto)
    {
    case protocol::not_care: hints.ai_family = AF_UNSPEC; break;
    case protocol::ipv4: hints.ai_family = AF_INET; break;
    case protocol::ipv6: hints.ai_family = AF_INET6; break;
    }

    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* servinfo = nullptr;
    int       sts      = ::getaddrinfo(!host.empty() ? host.data() : nullptr, port.data(), &hints, &servinfo);
    if (sts != 0) throw_for_gai_error(sts);

    // find first valid addr, and use it!
    int fd = -1;

    for (::addrinfo* info = servinfo; info != nullptr; info = info->ai_next)
    {
        fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd > -1) break;
    }

    freeaddrinfo(servinfo);

    if (fd == -1) throw exception{"failed to bind"};

    return fd;
}

}
