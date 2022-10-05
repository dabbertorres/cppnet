#include "tcp.hpp"

#include <algorithm>
#include <cerrno>

#include <netdb.h>
#include <unistd.h>

#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "exception.hpp"

namespace net
{

tcp_socket::tcp_socket(int fd)
    : socket(fd)
{}

tcp_socket::tcp_socket(
    std::string_view host, std::string_view port, protocol proto, bool keepalive, std::chrono::microseconds timeout)
    : socket(invalid_fd)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
    addrinfo hints = {0};
#pragma clang diagnostic pop

    switch (proto)
    {
    case protocol::not_care: hints.ai_family = AF_UNSPEC; break;
    case protocol::ipv4: hints.ai_family = AF_INET; break;
    case protocol::ipv6: hints.ai_family = AF_INET6; break;
    }

    hints.ai_flags    = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* servinfo = nullptr;
    auto      sts      = ::getaddrinfo(!host.empty() ? host.data() : nullptr, port.data(), &hints, &servinfo);
    if (sts != 0) throw_for_gai_error(sts);

    // find first valid addr, and use it!

    for (auto* info = servinfo; info != nullptr; info = info->ai_next)
    {
        fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd < 0) continue;

        struct timeval tv = {
            .tv_sec  = 0,
            .tv_usec = static_cast<decltype(tv.tv_usec)>(timeout.count()),
        };

        if (!set_option(SO_RCVTIMEO, &tv))
        {
            ::close(fd);
            fd = invalid_fd;
            continue;
        }

        if (keepalive)
        {
            int yes = 1;
            if (!set_option(SO_RCVTIMEO, &yes))
            {
                ::close(fd);
                fd = invalid_fd;
                continue;
            }
        }

        if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
        {
            ::close(fd);
            fd = invalid_fd;
            continue;
        }

        sts = ::connect(fd, info->ai_addr, info->ai_addrlen);
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

}
