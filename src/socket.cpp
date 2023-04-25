#include "socket.hpp"

#include <cassert>
#include <cerrno>
#include <iostream>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "exception.hpp"

namespace net
{

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

socket::~socket() { close(); }

bool socket::valid() const noexcept
{
    if (fd == invalid_fd) return false;

    int       error_code;
    socklen_t error_code_size = sizeof(error_code);

    int sts = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error_code, &error_code_size);
    return sts != -1 && error_code == 0;
}

std::string addr_name(sockaddr_storage* addr)
{
    std::string ret;
    void*       addr_type = nullptr;
    switch (addr->ss_family)
    {
    case AF_INET:
        ret.resize(INET_ADDRSTRLEN);
        addr_type = &reinterpret_cast<sockaddr_in*>(addr)->sin_addr;
        break;

    case AF_INET6:
        ret.resize(INET6_ADDRSTRLEN);
        addr_type = &reinterpret_cast<sockaddr_in6*>(addr)->sin6_addr;
        break;

    default:
        [[unlikely]]
#ifdef __cpp_lib_unreachable
        std::unreachable()
#endif
            ;
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

io::result socket::read(io::byte* data, size_t length) noexcept
{
    size_t received = 0;
    size_t attempts = 0;

    while (true)
    {
        const int64_t num = ::recv(fd, data + received, length - received, 0);
        if (num < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                ++attempts;

                if (attempts < 5) continue;
                return {.count = received};
            }

            return {
                .count = received,
                .err   = std::error_condition{errno, std::system_category()}
            };
        }

        // client closed connection
        if (num == 0) return {.count = received};

        received += static_cast<size_t>(num);
        break;
    }
    return {.count = received};
}

io::result socket::write(const io::byte* data, size_t length) noexcept
{
    size_t sent = 0;
    while (sent < length)
    {
        /* ::select(int, fd_set *, fd_set *, fd_set *, struct timeval *); */
        /* ::pselect(int, fd_set *, fd_set *, fd_set *, const struct timespec *, const sigset_t *); */

        const int64_t num = ::send(fd, data + sent, length - sent, 0);
        if (num < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return {
                .count = sent,
                .err   = std::error_condition{errno, std::system_category()}
            };
        }

        // client closed connection
        if (num == 0) return {.count = sent};

        sent += static_cast<size_t>(num);
    }
    return {.count = sent};
}

void socket::close(bool graceful, std::chrono::seconds graceful_timeout) const noexcept
{
    if (!valid()) return;

    if (graceful)
    {
        linger linger = {
            .l_onoff  = 1,
            .l_linger = static_cast<int>(graceful_timeout.count()),
        };
        set_option(fd, SO_LINGER, &linger);
    }

    ::close(fd);
}

}
