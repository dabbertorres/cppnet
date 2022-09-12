#include "socket.hpp"

#include <cerrno>
#include <iostream>

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "exception.hpp"

namespace net
{

socket::socket()
    : socket(invalid_fd)
{}

socket::socket(int fd)
    : fd{fd}
{}

socket::socket(socket&& other) noexcept
    : fd{other.fd}
{
    other.fd = invalid_fd;
}

socket& socket::operator=(socket&& other) noexcept
{
    if (fd != invalid_fd) ::close(fd);

    fd       = other.fd;
    other.fd = invalid_fd;

    return *this;
}

socket::~socket()
{
    if (valid()) ::close(fd);
}

bool socket::valid() const noexcept { return fd != invalid_fd; }

std::string addr_name(sockaddr_storage* addr)
{
    std::string ret;
    const void* addr_type = nullptr;
    switch (addr->ss_family)
    {
    case AF_INET:
        ret.resize(INET_ADDRSTRLEN);
        addr_type = &reinterpret_cast<sockaddr_in*>(&addr)->sin_addr;
        break;

    case AF_INET6:
        ret.resize(INET6_ADDRSTRLEN);
        addr_type = &reinterpret_cast<sockaddr_in6*>(&addr)->sin6_addr;
        break;
    }

    const char* ptr = inet_ntop(addr->ss_family, addr_type, ret.data(), static_cast<socklen_t>(ret.size()));
    if (ptr == nullptr) throw system_error_from_errno(errno);

    return ret;
}

std::string socket::local_addr() const
{
    sockaddr_storage addr;
    socklen_t        size = sizeof(addr);

    int sts = getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &size);
    if (sts != 0) throw system_error_from_errno(errno);
    return addr_name(&addr);
}

std::string socket::remote_addr() const
{
    sockaddr_storage addr;
    socklen_t        size = sizeof(addr);

    int sts = getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &size);
    if (sts != 0) throw system_error_from_errno(errno);
    return addr_name(&addr);
}

io_result socket::read(std::byte* data, size_t length) noexcept
{
    size_t rcvd = 0;
    while (rcvd < length)
    {
        int64_t num = ::recv(fd, data + rcvd, length - rcvd, 0);
        if (num < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return {
                .count = rcvd,
                .err   = std::error_condition{errno, std::system_category()}
            };
        }
        rcvd += num;
    }
    return {.count = rcvd};
}

io_result socket::write(const std::byte* data, size_t length) noexcept
{
    size_t sent = 0;
    while (sent < length)
    {
        int64_t num = ::send(fd, data + sent, length - sent, 0);
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return {
                .count = sent,
                .err   = std::error_condition{errno, std::system_category()}
            };
        }
        sent += num;
    }
    return {.count = sent};
}

}
