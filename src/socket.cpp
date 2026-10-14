#include "socket.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "coro/task.hpp"
#include "exception.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"
#include "io/scheduler.hpp"

namespace
{

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
    if (ptr == nullptr) throw net::system_error_from_errno(errno);

    return ret;
}

}

namespace net
{

socket::socket(io::scheduler* scheduler, io::handle fd)
    : scheduler{scheduler}
    , fd{fd}
{
    if (fd == invalid_fd) return;

    // The scheduler only says when the socket is ready, so reads and writes must never block. Also, don't leak the
    // socket into child processes.
    int flags = ::fcntl(fd, F_GETFL);
    if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
    {
        auto err = errno;
        ::close(fd);
        throw system_error_from_errno(err, "failed to make socket non-blocking");
    }

#ifdef SO_NOSIGPIPE
    // writing to a closed socket would otherwise kill the process with SIGPIPE (Linux uses MSG_NOSIGNAL instead).
    int yes = 1;
    set_option(fd, SO_NOSIGPIPE, &yes);
#endif

    try
    {
        if (scheduler != nullptr) scheduler->register_handle(fd);
    }
    catch (...)
    {
        ::close(fd);
        throw;
    }
}

socket::socket(socket&& other) noexcept
    : scheduler{std::exchange(other.scheduler, nullptr)}
    , fd{std::exchange(other.fd, invalid_fd)}
    , read_deadline{std::exchange(other.read_deadline, std::nullopt)}
    , write_deadline{std::exchange(other.write_deadline, std::nullopt)}
{}

socket& socket::operator=(socket&& other) noexcept
{
    if (this != &other)
    {
        close(false);

        scheduler      = std::exchange(other.scheduler, nullptr);
        fd             = std::exchange(other.fd, invalid_fd);
        read_deadline  = std::exchange(other.read_deadline, std::nullopt);
        write_deadline = std::exchange(other.write_deadline, std::nullopt);
    }

    return *this;
}

socket::~socket() { close(); }

bool socket::valid() const noexcept
{
    if (fd == invalid_fd || scheduler == nullptr) return false;

    int       error_code; // NOLINT(cppcoreguidelines-init-variables)
    socklen_t error_code_size = sizeof(error_code);

    int sts = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error_code, &error_code_size);
    return sts != -1 && error_code == 0;
}

std::string socket::local_addr() const
{
    sockaddr_storage addr{};
    socklen_t        size = sizeof(addr);

    int sts = getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &size);
    if (sts != 0) throw system_error_from_errno(errno, "failed to get local address");
    return addr_name(&addr);
}

std::string socket::remote_addr() const
{
    sockaddr_storage addr{};
    socklen_t        size = sizeof(addr);

    int sts = getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &size);
    if (sts != 0) throw system_error_from_errno(errno, "failed to get remote address");
    return addr_name(&addr);
}

void socket::set_deadline(std::optional<io::clock::time_point> deadline) noexcept
{
    read_deadline  = deadline;
    write_deadline = deadline;
}

void socket::set_read_deadline(std::optional<io::clock::time_point> deadline) noexcept { read_deadline = deadline; }

void socket::set_write_deadline(std::optional<io::clock::time_point> deadline) noexcept { write_deadline = deadline; }

coro::task<net::io::result> socket::read(std::span<std::byte> data) noexcept
{
    if (data.empty()) co_return {.count = 0};
    if (fd == invalid_fd || scheduler == nullptr)
        co_return {.count = 0, .err = std::make_error_condition(std::errc::bad_file_descriptor)};

    while (true)
    {
        const auto num = ::recv(fd, data.data(), data.size(), 0);

        if (num > 0) co_return {.count = static_cast<std::size_t>(num)};

        // the other end closed the connection
        if (num == 0) co_return {.count = 0, .err = make_error_condition(io::status_condition::closed)};

        auto err = errno;
        if (err == EINTR) continue;
        if (err != EAGAIN && err != EWOULDBLOCK) co_return {.count = 0, .err = {err, std::generic_category()}};

        auto wait_err = co_await scheduler->wait_until(fd, io::poll_op::read, read_deadline);
        if (wait_err) co_return {.count = 0, .err = wait_err};
    }
}

coro::task<io::result> socket::write(std::span<const std::byte> data) noexcept
{
    if (fd == invalid_fd || scheduler == nullptr)
        co_return {.count = 0, .err = std::make_error_condition(std::errc::bad_file_descriptor)};

#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL; // don't raise SIGPIPE when writing to a closed socket.
#else
    constexpr int flags = 0;
#endif

    std::size_t total_written = 0;

    while (total_written < data.size())
    {
        const auto num = ::send(fd, data.data() + total_written, data.size() - total_written, flags);

        if (num >= 0)
        {
            total_written += static_cast<std::size_t>(num);
            continue;
        }

        auto err = errno;
        if (err == EINTR) continue;
        if (err != EAGAIN && err != EWOULDBLOCK)
            co_return {.count = total_written, .err = {err, std::generic_category()}};

        auto wait_err = co_await scheduler->wait_until(fd, io::poll_op::write, write_deadline);
        if (wait_err) co_return {.count = total_written, .err = wait_err};
    }

    co_return {.count = total_written};
}

void socket::close(bool graceful, std::chrono::seconds graceful_timeout) noexcept
{
    // NOTE: not valid(), as that is also false for a socket with a pending error, which must still be closed.
    if (fd == invalid_fd) return;

    if (graceful)
    {
        struct linger linger = {
            .l_onoff  = 1,
            .l_linger = static_cast<int>(graceful_timeout.count()),
        };
        set_option(fd, SO_LINGER, &linger);
    }

    // deregister first: it cancels any waits, and epoll needs the handle to still be open.
    if (scheduler != nullptr) scheduler->deregister_handle(fd);

    // forget the fd, so it isn't closed again later (by which time the number may belong to another file).
    ::close(std::exchange(fd, invalid_fd));
}

}
