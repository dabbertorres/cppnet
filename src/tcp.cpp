#include "tcp.hpp"

#include <cerrno>
#include <chrono>
#include <string>
#include <string_view>

#include <netdb.h>
#include <sys/_select.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <netinet/in.h>

#include "exception.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "socket.hpp"
#include "util/defer.hpp"

namespace net
{

using namespace std::string_literals;

int tcp_socket::open(
    std::string_view host, std::string_view port, protocol proto, bool keepalive, std::chrono::microseconds timeout)
{
    // TODO: should be using net::dns_lookup() instead of getaddrinfo()...

    addrinfo hints = {};

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

    util::defer freeservinfo{[=] noexcept { freeaddrinfo(servinfo); }};

    // find first valid addr, and use it!
    int fd       = invalid_fd;
    int last_err = 0;

    for (auto* info = servinfo; info != nullptr; info = info->ai_next)
    {
        fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd == -1)
        {
            last_err = errno;
            continue;
        }

        {
            int yes = 1;
            if (!set_option(fd, SO_REUSEADDR, &yes))
            {
                auto err = errno;
                ::close(fd);
                throw system_error_from_errno(err, "failed to set reuseaddr");
            }
        }

        if (timeout.count() != 0)
        {
            auto sec  = std::chrono::duration_cast<std::chrono::seconds>(timeout);
            auto usec = timeout - sec;

            struct timeval tv = {
                .tv_sec  = static_cast<decltype(tv.tv_sec)>(sec.count()),
                .tv_usec = static_cast<decltype(tv.tv_usec)>(usec.count()),
            };

            if (!set_option(fd, SO_RCVTIMEO, &tv))
            {
                auto err = errno;
                ::close(fd);
                throw system_error_from_errno(err, "failed to set receive timeout");
            }
        }

        if (keepalive)
        {
            int yes = 1;
            if (!set_option(fd, SO_KEEPALIVE, &yes))
            {
                auto err = errno;
                ::close(fd);
                throw system_error_from_errno(err, "failed to set keepalive");
            }
        }

        sts = ::connect(fd, info->ai_addr, info->ai_addrlen);
        if (sts == 0) break;

        last_err = errno;

        ::close(fd);
        fd = invalid_fd;
    }

    if (fd == invalid_fd) throw system_error_from_errno(last_err, "failed to open and connect to socket");

    return fd;
}

tcp_socket::tcp_socket(io::scheduler* scheduler) noexcept
    : socket{scheduler, invalid_fd}
{}

tcp_socket::tcp_socket(io::scheduler* scheduler, int fd) noexcept
    : socket{scheduler, fd}
{}

tcp_socket::tcp_socket(io::scheduler*            scheduler,
                       std::string_view          host,
                       std::string_view          port,
                       protocol                  proto,
                       bool                      keepalive,
                       std::chrono::microseconds timeout)
    : socket{scheduler, open(host, port, proto, keepalive, timeout)}
{}

}
