#include "listen.hpp"

#include <cerrno>
#include <system_error>

#include <fcntl.h>
#include <netdb.h>
/* #include <poll.h> */
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#include "exception.hpp"
#include "tcp.hpp"

namespace net
{

constexpr int invalid_fd = -1;

listener::listener(
    const std::string& host, const std::string& port, network net, protocol proto, std::chrono::microseconds timeout)
    : main_fd{invalid_fd}
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
    default: throw exception{"invalid protocol"};
    }

    if (host.empty()) hints.ai_flags = AI_PASSIVE;

    switch (net)
    {
    case network::tcp: hints.ai_socktype = SOCK_STREAM; break;
    case network::udp: hints.ai_socktype = SOCK_DGRAM; break;
    default: throw exception{"invalid network type"};
    }

    addrinfo* servinfo = nullptr;
    int       sts      = ::getaddrinfo(!host.empty() ? host.data() : nullptr, port.data(), &hints, &servinfo);
    if (sts != 0) throw_for_gai_error(sts);

    // find first valid addr, and use it

    for (addrinfo* info = servinfo; info != nullptr; info = info->ai_next)
    {
        main_fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (main_fd < 0) continue;

        int yes = 1;
        sts     = ::setsockopt(main_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
        if (sts != 0)
        {
            ::close(main_fd);
            main_fd = invalid_fd;
            continue;
        }

        /* struct timeval tv = { */
        /*     .tv_sec  = 0, */
        /*     .tv_usec = static_cast<decltype(tv.tv_usec)>(timeout.count()), */
        /* }; */
        /* sts = ::setsockopt(main_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); */
        /* if (sts != 0) */
        /* { */
        /*     ::close(main_fd); */
        /*     main_fd = invalid_fd; */
        /*     continue; */
        /* } */

        /* int flags = ::fcntl(main_fd, F_GETFL); */
        /* if (flags != 0) */
        /* { */
        /*     ::close(main_fd); */
        /*     main_fd = invalid_fd; */
        /*     continue; */
        /* } */

        /* flags |= O_NONBLOCK; */
        /* sts = ::fcntl(main_fd, F_SETFL, flags); */
        /* if (sts != 0) */
        /* { */
        /*     ::close(main_fd); */
        /*     main_fd = invalid_fd; */
        /*     continue; */
        /* } */

        sts = ::bind(main_fd, info->ai_addr, info->ai_addrlen);
        if (sts != 0)
        {
            ::close(main_fd);
            main_fd = invalid_fd;
            continue;
        }

        break;
    }

    ::freeaddrinfo(servinfo);

    if (main_fd == invalid_fd) throw exception{"failed to bind a socket matching options"};
    // TODO report the reasons why we couldn't bind a socket?
}

listener::listener(listener&& other) noexcept
    : is_listening(other.is_listening.exchange(false))
    , main_fd(other.main_fd)
{
    other.main_fd = invalid_fd;
}

listener& listener::operator=(listener&& other) noexcept
{
    if (is_listening && main_fd != invalid_fd)
    {
        is_listening = false;
        is_listening.notify_all();
        ::close(main_fd);
    }

    is_listening  = other.is_listening.exchange(false);
    main_fd       = other.main_fd;
    other.main_fd = invalid_fd;

    return *this;
}

listener::~listener() noexcept
{
    if (is_listening && main_fd != invalid_fd)
    {
        is_listening = false;
        is_listening.notify_all();
        ::close(main_fd);
    }
}

void listener::listen(uint16_t max_backlog)
{
    if (is_listening.exchange(true))
    {
        // TODO: throw "already listening"
        return;
    }

    int res = ::listen(main_fd, max_backlog);
    if (res == -1) throw system_error_from_errno(errno);
    /* fds.push_back(pollfd{ */
    /*     .fd     = main_fd, */
    /*     .events = POLLIN, */
    /* }); */
}

tcp_socket listener::accept() const
{
    if (!is_listening) return tcp_socket{invalid_fd};

    /* int num_ready = */
    /*     ::poll(fds.data(), static_cast<unsigned int>(fds.size()), -1); // TODO block? not-block?
     */
    /* if (num_ready == -1) throw system_error_from_errno(errno); */
    /* if (num_ready == 0) return tcp_socket{invalid_fd}; */

    sockaddr_storage inc;
    socklen_t        inc_size = sizeof(inc);

    int inc_fd = ::accept(main_fd, reinterpret_cast<sockaddr*>(&inc), &inc_size);
    if (inc_fd == -1) throw system_error_from_errno(errno);

    return tcp_socket{inc_fd};
}

}
