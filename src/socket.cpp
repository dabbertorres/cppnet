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

socket::socket() : fd{invalid_fd} {}

socket::socket(size_t buf_size) : fd{invalid_fd}, read_buf(buf_size), write_buf(buf_size) {}

socket::socket(int fd, size_t buf_size) : fd{fd}, read_buf(buf_size), write_buf(buf_size)
{
    setg(read_buf.data(), read_buf.data(), &read_buf.back());
    setp(write_buf.data(), &write_buf.back());
}

socket::socket(socket&& other) noexcept :
    fd{other.fd},
    read_buf{std::move(other.read_buf)},
    write_buf{std::move(other.write_buf)}
{
    this->swap(other);
    other.fd = invalid_fd;
}

socket& socket::operator=(socket&& other) noexcept
{
    if (fd != invalid_fd) ::close(fd);

    fd       = other.fd;
    other.fd = invalid_fd;

    read_buf  = std::move(other.read_buf);
    write_buf = std::move(other.write_buf);
    this->swap(other);

    return *this;
}

socket::~socket()
{
    if (fd != invalid_fd) ::close(fd);
}

bool socket::valid() const noexcept { return fd != invalid_fd; }

std::string addr_name(sockaddr* addr)
{
    std::string ret;
    const void* addr_type = nullptr;
    switch (addr->sa_family)
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

    auto ptr = inet_ntop(addr->sa_family, addr_type, ret.data(), ret.size());
    if (ptr == nullptr) throw exception{};

    return ret;
}

std::string socket::local_addr() const
{
    sockaddr  addr;
    socklen_t size = sizeof(addr);

    int sts = getsockname(fd, &addr, &size);
    if (sts != 0) throw exception{};
    return addr_name(&addr);
}

std::string socket::remote_addr() const
{
    sockaddr  addr;
    socklen_t size = sizeof(addr);

    int sts = getpeername(fd, &addr, &size);
    if (sts != 0) throw exception{};
    return addr_name(&addr);
}

int socket::underflow()
{
    if (read(read_buf.data(), read_buf.size()))
        return read_buf[0];
    else
        return std::char_traits<streambuf::char_type>::eof();

    setg(read_buf.data(), read_buf.data(), &read_buf.back() + 1);
    return read_buf.at(0);
}

int socket::overflow(int ch)
{
    if (write(write_buf.data(), write_buf.size()))
        return 1;
    else
        return std::char_traits<streambuf::char_type>::eof();

    if (ch != std::char_traits<streambuf::char_type>::eof()) write_buf[0] = ch;

    setp(write_buf.data(), &write_buf.back() + 1);
    return 1;
}

int socket::sync()
{
    // don't care if failure
    underflow();
    overflow(std::char_traits<streambuf::char_type>::eof());
    return 0;
}

}
