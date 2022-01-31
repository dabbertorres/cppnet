#include "tcp.hpp"

#include <algorithm>
#include <cerrno>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace net
{

tcp_socket::tcp_socket(int fd, size_t buf_size)
    : socket(fd, buf_size)
{}

tcp_socket::tcp_socket(std::string_view host, std::string_view port, addr_protocol proto, size_t buf_size, std::chrono::microseconds timeout)
    : socket(buf_size)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
    addrinfo hints = {0};
#pragma clang diagnostic pop

    switch (proto)
    {
    case addr_protocol::not_care:
        hints.ai_family = AF_UNSPEC;
        break;

    case addr_protocol::ipv4:
        hints.ai_family = AF_INET;
        break;

    case addr_protocol::ipv6:
        hints.ai_family = AF_INET6;
        break;
    }

    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *servinfo;
    int sts = ::getaddrinfo(host != "" ? host.data() : nullptr, port.data(), &hints, &servinfo);
    if (sts != 0)
        throw exception{sts};

    // find first valid addr, and use it!

    for (addrinfo* info = servinfo; info != nullptr; info = info->ai_next)
    {
        fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd < 0)
            continue;

        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = static_cast<decltype(tv.tv_usec)>(timeout.count()),
        };
        sts = ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        if (sts != 0)
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

    if (fd == invalid_fd)
        throw exception{"failed to bind"};
}

size_t tcp_socket::read(uint8_t* data, size_t length) noexcept
{
    size_t rcvd = 0;
    for (;;)
    {
        int num = ::recv(fd, data + rcvd, length - rcvd, 0);
        if (static_cast<size_t>(num) == length) return length;
        if (num > 0)
        {
            rcvd += num;
            continue;
        }

        if (num <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            else
                return 0;
        }
    }
}

size_t tcp_socket::write(const uint8_t* data, size_t length) noexcept
{
    size_t sent = 0;
    for (;;)
    {
        int num = ::send(fd, data + sent, length - sent, 0);
        if (static_cast<size_t>(num) == length) return length;
        if (num > 0)
        {
            sent += num;
            continue;
        }

        if (num <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            else
                return 0;
        }
    }
}

}
