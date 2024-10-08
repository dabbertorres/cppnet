#include "listen.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "coro/task.hpp"
#include "exception.hpp"
#include "io/poll.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "tcp.hpp"

namespace net
{

constexpr int invalid_fd = -1;

listener::listener(io::scheduler*            scheduler,
                   const std::string&        host,
                   const std::string&        port,
                   network                   net,
                   protocol                  proto,
                   std::chrono::microseconds timeout)
    : scheduler{scheduler}
    , main_fd{invalid_fd}
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

        sts = ::bind(main_fd, info->ai_addr, info->ai_addrlen);
        if (sts != 0)
        {
            ::close(main_fd);
            main_fd = invalid_fd;
            continue;
        }

        break;
    }

    auto last_err = errno;

    ::freeaddrinfo(servinfo);

    if (main_fd == invalid_fd) throw system_error_from_errno(last_err, "failed to bind a socket matching options");

    scheduler->register_handle(main_fd);
}

listener::listener(listener&& other) noexcept
    : scheduler{std::exchange(other.scheduler, nullptr)}
    , is_listening{other.is_listening.exchange(false, std::memory_order::acq_rel)}
    , main_fd{std::exchange(other.main_fd, invalid_fd)}
{}

listener& listener::operator=(listener&& other) noexcept
{
    shutdown();

    scheduler    = std::exchange(other.scheduler, nullptr);
    is_listening = other.is_listening.exchange(false, std::memory_order::acq_rel);
    main_fd      = std::exchange(other.main_fd, invalid_fd);

    return *this;
}

listener::~listener() noexcept { shutdown(); }

void listener::listen(std::uint16_t max_backlog)
{
    if (is_listening.exchange(true, std::memory_order::acq_rel))
    {
        // TODO: throw "already listening"
        return;
    }

    int res = ::listen(main_fd, max_backlog);
    if (res == -1) throw system_error_from_errno(errno, "failed to listen");
    /* fds.push_back(pollfd{ */
    /*     .fd     = main_fd, */
    /*     .events = POLLIN, */
    /* }); */
}

coro::task<tcp_socket> listener::accept() const
{
    if (!is_listening.load(std::memory_order::acquire)) throw exception{"not listening"};

    sockaddr_storage inc{};
    socklen_t        inc_size = sizeof(inc);

    auto res = co_await scheduler->schedule(native_handle(), io::poll_op::read, 0ms);
    if (res.err) throw res.err;
    if (res.count == 0) co_return tcp_socket{scheduler};

    int inc_fd = ::accept(main_fd, reinterpret_cast<sockaddr*>(&inc), &inc_size);
    if (inc_fd == -1)
    {
        // return invalid socket to indicate shutdown
        if (!is_listening.load(std::memory_order::acquire)) co_return tcp_socket{scheduler};

        throw system_error_from_errno(errno);
    }

    co_return tcp_socket{scheduler, inc_fd};
}

void listener::shutdown() noexcept
{
    if (is_listening.exchange(false, std::memory_order::acq_rel) && main_fd != invalid_fd)
    {
        ::close(main_fd);
        scheduler->deregister_handle(main_fd);
        main_fd = invalid_fd;
    }
}

}
