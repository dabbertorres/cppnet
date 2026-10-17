#include "listen.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <stdexcept>
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

constexpr net::io::handle invalid_fd = -1;

std::expected<net::io::handle, std::error_code>
listener::open(io::scheduler* scheduler, const std::string& host, const std::string& port, network net, protocol proto)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
    addrinfo hints = {.ai_flags = 0};
#pragma clang diagnostic pop

    switch (proto)
    {
    case protocol::not_care: hints.ai_family = AF_UNSPEC; break;
    case protocol::ipv4: hints.ai_family = AF_INET; break;
    case protocol::ipv6: hints.ai_family = AF_INET6; break;
    default: return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    if (host.empty()) hints.ai_flags = AI_PASSIVE;

    switch (net)
    {
    case network::tcp: hints.ai_socktype = SOCK_STREAM; break;
    case network::udp: hints.ai_socktype = SOCK_DGRAM; break;
    default: return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    addrinfo* servinfo = nullptr;
    int       sts      = ::getaddrinfo(!host.empty() ? host.data() : nullptr, port.data(), &hints, &servinfo);
    if (sts != 0) return std::unexpected(make_gai_error_code(sts));

    // find first valid addr, and use it
    net::io::handle fd       = invalid_fd;
    int last_err = 0;
    for (const addrinfo* info = servinfo; info != nullptr; info = info->ai_next)
    {
        fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
        if (fd < 0)
        {
            last_err = errno;
            continue;
        }

        int yes = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) != 0
            || ::bind(fd, info->ai_addr, info->ai_addrlen) != 0)
        {
            last_err = errno;
            ::close(std::exchange(fd, invalid_fd));
            continue;
        }

        break;
    }

    ::freeaddrinfo(servinfo);

    if (fd == invalid_fd) return std::unexpected(std::error_code{last_err, std::system_category()});

    // accept() must never block (the scheduler only says a connection may be ready), and the socket shouldn't leak into
    // child processes.
    int flags = ::fcntl(fd, F_GETFL);
    if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
    {
        auto err = errno;
        ::close(fd);
        return std::unexpected(std::error_code{err, std::system_category()});
    }

    try
    {
        scheduler->register_handle(fd);
    }
    catch (const std::system_error& ex)
    {
        ::close(fd);
        return std::unexpected(ex.code());
    }

    return fd;
}

std::expected<listener, std::error_code> listener::make(io::scheduler*     scheduler,
                                                        const std::string& host,
                                                        const std::string& port,
                                                        network            net,
                                                        protocol           proto,
                                                        std::uint16_t      max_backlog)
{
    auto fd = open(scheduler, host, port, net, proto);
    if (!fd.has_value()) return std::unexpected(fd.error());

    listener out{scheduler, *fd};
    if (net == network::tcp)
    {
        if (::listen(out.main_fd, max_backlog) == -1)
            return std::unexpected(std::error_code{errno, std::system_category()});
        out.is_listening.store(true, std::memory_order::release);
    }

    return out;
}

listener::listener(io::scheduler* scheduler, io::handle fd) noexcept
    : scheduler{scheduler}
    , is_listening{false}
    , main_fd{fd}
{}

listener::listener(
    io::scheduler* scheduler, const std::string& host, const std::string& port, network net, protocol proto)
    : scheduler{scheduler}
    , is_listening{false}
    , main_fd{invalid_fd}
{
    auto fd = open(scheduler, host, port, net, proto);
    if (!fd.has_value()) throw std::system_error{fd.error(), "failed to bind a listener"};
    main_fd = *fd;
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
    if (is_listening.exchange(true, std::memory_order::acq_rel)) throw std::logic_error{"already listening"};

    int res = ::listen(main_fd, max_backlog);
    if (res == -1)
    {
        const int err = errno;
        is_listening.store(false, std::memory_order::release);
        throw system_error_from_errno(err, "failed to listen");
    }
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

coro::task<std::expected<tcp_socket, std::error_code>> listener::accept() const
{
    const auto canceled = std::make_error_code(std::errc::operation_canceled);

    while (true)
    {
        if (!is_listening.load(std::memory_order::acquire)) co_return std::unexpected(canceled);

        io::handle inc_fd = ::accept(main_fd, nullptr, nullptr);
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

        default: co_return std::unexpected(std::error_code{err, std::system_category()});
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
