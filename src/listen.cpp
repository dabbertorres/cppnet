#include "listen.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

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

    // accept() must never block (the scheduler only says a connection may be ready), and the socket shouldn't leak into
    // child processes.
    int flags = ::fcntl(main_fd, F_GETFL);
    if (flags == -1 || ::fcntl(main_fd, F_SETFL, flags | O_NONBLOCK) == -1
        || ::fcntl(main_fd, F_SETFD, FD_CLOEXEC) == -1)
    {
        auto err = errno;
        ::close(std::exchange(main_fd, invalid_fd));
        throw system_error_from_errno(err, "failed to make listener non-blocking");
    }

    try
    {
        scheduler->register_handle(main_fd);
    }
    catch (...)
    {
        ::close(std::exchange(main_fd, invalid_fd));
        throw;
    }
}

listener::listener(listener&& other) noexcept
    : scheduler{std::exchange(other.scheduler, nullptr)}
    , is_listening{other.is_listening.exchange(false, std::memory_order::acq_rel)}
    , main_fd{std::exchange(other.main_fd, invalid_fd)}
{}

listener& listener::operator=(listener&& other) noexcept
{
    if (this != &other)
    {
        close();

        scheduler    = std::exchange(other.scheduler, nullptr);
        is_listening = other.is_listening.exchange(false, std::memory_order::acq_rel);
        main_fd      = std::exchange(other.main_fd, invalid_fd);
    }

    return *this;
}

listener::~listener() noexcept { close(); }

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

std::uint16_t listener::local_port() const
{
    sockaddr_storage addr{};
    socklen_t        size = sizeof(addr);
    if (::getsockname(main_fd, reinterpret_cast<sockaddr*>(&addr), &size) == -1)
    {
        throw system_error_from_errno(errno, "failed to get the listener's address");
    }

    switch (addr.ss_family)
    {
    case AF_INET: return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
    case AF_INET6: return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);
    default: throw exception{"the listener's address isn't IPv4 or IPv6"};
    }
}

coro::task<std::expected<tcp_socket, std::error_condition>> listener::accept() const
{
    const auto canceled = std::make_error_condition(std::errc::operation_canceled);

    while (true)
    {
        if (!is_listening.load(std::memory_order::acquire)) co_return std::unexpected(canceled);

        int inc_fd = ::accept(main_fd, nullptr, nullptr);
        if (inc_fd != -1) co_return tcp_socket{scheduler, inc_fd};

        auto err = errno;
        switch (err)
        {
        case EINTR: [[fallthrough]];
        case ECONNABORTED: continue; // the connection was reset before we got to it: try the next one.

        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif
        {
            auto wait_err = co_await scheduler->wait(main_fd, io::poll_op::read);
            if (wait_err)
            {
                // shutdown() deregisters the handle, which may be seen as either error.
                if (!is_listening.load(std::memory_order::acquire)) co_return std::unexpected(canceled);
                co_return std::unexpected(wait_err);
            }

            continue;
        }

        default: co_return std::unexpected(std::error_condition{err, std::generic_category()});
        }
    }
}

void listener::shutdown() noexcept
{
    // NOTE: the handle stays open until the listener is destroyed, as accept() may be using it on another thread.
    if (is_listening.exchange(false, std::memory_order::acq_rel)) scheduler->deregister_handle(main_fd);
}

void listener::close() noexcept
{
    if (main_fd == invalid_fd) return;

    // NOTE: deregistered regardless of shutdown(), as the handle is registered before listen() is called.
    is_listening.store(false, std::memory_order::release);
    scheduler->deregister_handle(main_fd);
    ::close(std::exchange(main_fd, invalid_fd));
}

}
