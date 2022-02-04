#include "listen.hpp"

#include <cerrno>

#include <netdb.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#include "exception.hpp"
#include "tcp.hpp"

namespace net
{

constexpr int invalid_fd = -1;

listener::listener(std::string_view          host,
                   std::string_view          port,
                   network                   net,
                   addr_protocol             proto,
                   std::chrono::microseconds timeout) :
    fd{invalid_fd}
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
    addrinfo hints = {0};
#pragma clang diagnostic pop

    switch (proto)
    {
    case addr_protocol::not_care: hints.ai_family = AF_UNSPEC; break;

    case addr_protocol::ipv4: hints.ai_family = AF_INET; break;

    case addr_protocol::ipv6: hints.ai_family = AF_INET6; break;
    }

    if (host == "") hints.ai_flags = AI_PASSIVE;

    switch (net)
    {
    case network::tcp: hints.ai_socktype = SOCK_STREAM; break;

    case network::udp: hints.ai_socktype = SOCK_DGRAM; break;
    }

    addrinfo* servinfo;
    int sts = ::getaddrinfo(host != "" ? host.data() : nullptr, port.data(), &hints, &servinfo);
    if (sts != 0) throw exception{sts};

    // find first valid addr, and use it!

    for (addrinfo* info = servinfo; info != nullptr; info = info->ai_next)
    {
        fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd < 0) continue;

        int yes = 1;
        sts     = ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
        if (sts != 0)
        {
            ::close(fd);
            fd = invalid_fd;
            continue;
        }

        struct timeval tv = {
            .tv_sec  = 0,
            .tv_usec = static_cast<decltype(tv.tv_usec)>(timeout.count()),
        };
        sts = ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (sts != 0)
        {
            ::close(fd);
            fd = invalid_fd;
            continue;
        }

        sts = ::bind(fd, info->ai_addr, info->ai_addrlen);
        if (sts != 0)
        {
            ::close(fd);
            fd = invalid_fd;
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo);

    if (fd == invalid_fd) throw exception{"failed to bind"};
}

bool listener::listen(uint16_t max_pending) const noexcept
{
    return ::listen(fd, max_pending) == 0;
}

tcp_socket listener::accept() const
{
    sockaddr_storage inc;
    socklen_t        inc_size = sizeof(inc);

    int inc_fd = ::accept(fd, reinterpret_cast<sockaddr*>(&inc), &inc_size);
    return tcp_socket{inc_fd};
}

}
