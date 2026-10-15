#include "socket.hpp"

#include <algorithm>
#include <array>
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
#include <sys/un.h>
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

// wait_deadline is when a wait must end by: the deadline, or the timeout from now, whichever is sooner.
std::optional<net::io::clock::time_point> wait_deadline(std::optional<net::io::clock::time_point> deadline,
                                                        std::optional<net::io::clock::duration>   timeout) noexcept
{
    if (!timeout) return deadline;

    auto from_now = net::io::clock::now() + *timeout;
    return deadline ? std::min(*deadline, from_now) : from_now;
}

std::string addr_name(const sockaddr_storage* addr)
{
    std::array<char, INET6_ADDRSTRLEN> buf{};
    const void*                        addr_bytes = nullptr;

    switch (addr->ss_family)
    {
    case AF_INET: addr_bytes = &reinterpret_cast<const sockaddr_in*>(addr)->sin_addr; break;
    case AF_INET6: addr_bytes = &reinterpret_cast<const sockaddr_in6*>(addr)->sin6_addr; break;

    // a Unix domain socket's path, which is empty if it isn't bound to one (e.g. from socketpair()).
    case AF_UNIX:
    {
        // NOTE: sun_path needn't be null-terminated if it's full.
        const auto& path = reinterpret_cast<const sockaddr_un*>(addr)->sun_path;
        return {std::begin(path), std::ranges::find(path, '\0')};
    }

    default: throw net::exception{"unsupported address family: " + std::to_string(addr->ss_family)};
    }

    const char* ptr = ::inet_ntop(addr->ss_family, addr_bytes, buf.data(), static_cast<socklen_t>(buf.size()));
    if (ptr == nullptr) throw net::system_error_from_errno(errno);

    // NOTE: only up to the terminating null: the buffer is sized for the longest address.
    return ptr;
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
    , read_timeout{std::exchange(other.read_timeout, std::nullopt)}
    , write_timeout{std::exchange(other.write_timeout, std::nullopt)}
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
        read_timeout   = std::exchange(other.read_timeout, std::nullopt);
        write_timeout  = std::exchange(other.write_timeout, std::nullopt);
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

void socket::set_timeout(std::optional<io::clock::duration> timeout) noexcept
{
    read_timeout  = timeout;
    write_timeout = timeout;
}

void socket::set_read_timeout(std::optional<io::clock::duration> timeout) noexcept { read_timeout = timeout; }

void socket::set_write_timeout(std::optional<io::clock::duration> timeout) noexcept { write_timeout = timeout; }

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

        auto wait_err = co_await scheduler->wait_until(fd, io::poll_op::read, wait_deadline(read_deadline, read_timeout));
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

        auto wait_err =
            co_await scheduler->wait_until(fd, io::poll_op::write, wait_deadline(write_deadline, write_timeout));
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

void socket::shutdown_write() noexcept
{
    if (fd != invalid_fd) ::shutdown(fd, SHUT_WR);
}

}
