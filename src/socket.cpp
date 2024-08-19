#include "socket.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

socket::socket(io::scheduler* scheduler, io::io_handle fd)
    : scheduler{scheduler}
    , fd{fd}
{
    scheduler->register_handle(fd);
}

socket::socket(socket&& other) noexcept
    : scheduler{std::exchange(other.scheduler, nullptr)}
    , fd{std::exchange(other.fd, invalid_fd)}
{}

socket& socket::operator=(socket&& other) noexcept
{
    if (fd != invalid_fd) ::close(fd);

    fd        = std::exchange(other.fd, invalid_fd);
    scheduler = std::exchange(other.scheduler, nullptr);

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

io::result socket::read(std::span<std::byte> data) noexcept
{
    std::size_t received = 0;
    std::size_t attempts = 0;

    while (true)
    {
        const std::int64_t num = ::recv(fd, data.data() + received, data.size() - received, 0);
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
        if (num == 0)
        {
            return {
                .count = received,
                .err   = make_error_condition(io::status_condition::closed),
            };
        }

        received += static_cast<std::size_t>(num);
        break;
    }

    return {.count = received};
}

coro::task<net::io::result> socket::co_read(std::span<std::byte> data) noexcept
{
    auto res = co_await scheduler->schedule(native_handle(), io::poll_op::read, 0ms);
    if (res.err && res.count == 0) co_return res;

    auto read_amount = std::min(res.count, data.size());

    res = read(data.subspan(0, read_amount));
    co_return res;
}

io::result socket::write(std::span<const std::byte> data) noexcept
{
    std::size_t sent = 0;
    while (sent < data.size())
    {
        const std::int64_t num = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (num < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return {
                .count = sent,
                .err   = std::error_condition{errno, std::system_category()}
            };
        }

        // client closed connection
        if (num == 0) return {.count = sent, .err = make_error_condition(io::status_condition::closed)};

        sent += static_cast<std::size_t>(num);
    }

    return {.count = sent};
}

coro::task<io::result> socket::co_write(std::span<const std::byte> data) noexcept
{
    std::size_t total_written = 0;

    while (total_written < data.size())
    {
        auto res = co_await scheduler->schedule(native_handle(), io::poll_op::write, 0ms);
        if (res.err && res.count == 0) co_return res;

        auto write_amount = std::min(res.count, data.size() - total_written);
        res               = write(data.subspan(total_written, write_amount));
        if (res.err) co_return res;

        total_written += res.count;
    }

    co_return {.count = total_written};
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

    scheduler->deregister_handle(fd);
    ::close(fd);
}

}
