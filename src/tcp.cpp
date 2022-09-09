#include "tcp.hpp"

#include <algorithm>
#include <cerrno>

#include <netdb.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#include "exception.hpp"

namespace net
{

tcp_socket::tcp_socket(int fd, size_t buf_size)
    : socket(fd, buf_size)
{}

tcp_socket::tcp_socket(
    std::string_view host, std::string_view port, protocol proto, size_t buf_size, std::chrono::microseconds timeout)
    : socket(buf_size)
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
        sts = ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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

    if (fd == invalid_fd) throw exception{"failed to bind"};
}

io_result tcp_socket::read(uint8_t* data, size_t length) noexcept
{
    size_t received = 0;
    for (;;)
    {
        auto num = ::recv(fd, data + received, length - received, 0);
        if (num == static_cast<ssize_t>(length)) return {.count = length};
        if (num > 0)
        {
            received += static_cast<size_t>(num);
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;

        return {
            .count = received,
            .err   = std::error_condition{errno, std::system_category()},
        };
    }
}

io_result tcp_socket::write(const uint8_t* data, size_t length) noexcept
{
    size_t written = 0;
    for (;;)
    {
        auto num = ::send(fd, data + written, length - written, 0);
        if (num == static_cast<ssize_t>(length)) return {.count = length};
        if (num > 0)
        {
            written += static_cast<size_t>(num);
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;

        return {
            .count = written,
            .err   = std::error_condition{errno, std::system_category()},
        };
    }
}

std::error_condition tcp_socket::flush() noexcept
{
    // TODO
}

}
