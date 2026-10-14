#include "config.hpp"

#ifdef NET_HAS_EPOLL

#    include "io/epoll_loop.hpp"

// keep parent header above this

#    include <algorithm>
#    include <array>
#    include <cerrno>
#    include <chrono>
#    include <climits>
#    include <cstddef>
#    include <cstdint>
#    include <optional>
#    include <span>

#    include <sys/epoll.h>
#    include <sys/eventfd.h>
#    include <unistd.h>

#    include "exception.hpp"
#    include "io/event_loop.hpp"
#    include "io/io.hpp"
#    include "io/poll.hpp"

namespace
{

std::uint32_t to_epoll_events(net::io::poll_op op) noexcept
{
    // EPOLLONESHOT disables the handle once it has been reported, until it is armed again. It's also set when no
    // operations are wanted, as otherwise EPOLLHUP and EPOLLERR (which can't be masked) would be reported over and
    // over.
    std::uint32_t events = EPOLLONESHOT;
    if (net::io::is_readable(op)) events |= EPOLLIN | EPOLLRDHUP;
    if (net::io::is_writable(op)) events |= EPOLLOUT;
    return events;
}

net::io::poll_op from_epoll_events(std::uint32_t events) noexcept
{
    using net::io::poll_op;

    // hangups and errors are reported as ready: the next read or write will find out what happened.
    constexpr std::uint32_t closed = EPOLLHUP | EPOLLERR;

    auto op = poll_op::none;
    if ((events & (EPOLLIN | EPOLLRDHUP | closed)) != 0) op = op | poll_op::read;
    if ((events & (EPOLLOUT | closed)) != 0) op = op | poll_op::write;
    return op;
}

}

namespace net::io::detail
{

epoll_loop::epoll_loop()
    : epoll_fd{::epoll_create1(EPOLL_CLOEXEC)}
    , interrupt_fd{::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)}
{
    if (epoll_fd == -1 || interrupt_fd == -1)
    {
        auto err = errno;
        if (epoll_fd != -1) ::close(epoll_fd);
        if (interrupt_fd != -1) ::close(interrupt_fd);
        throw system_error_from_errno(err, "failed to create epoll");
    }

    epoll_event ev{.events = EPOLLIN, .data = {.fd = interrupt_fd}};
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, interrupt_fd, &ev) == -1)
    {
        auto err = errno;
        ::close(epoll_fd);
        ::close(interrupt_fd);
        throw system_error_from_errno(err, "failed to register epoll interrupt event");
    }
}

epoll_loop::~epoll_loop() noexcept
{
    ::close(epoll_fd);
    ::close(interrupt_fd);
}

// NOLINTNEXTLINE(readability-make-member-function-const): modifies the kernel object.
void epoll_loop::add(handle fd)
{
    epoll_event ev{.events = to_epoll_events(poll_op::none), .data = {.fd = fd}};
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1 && errno != EEXIST)
        throw system_error_from_errno(errno, "failed to add handle to epoll");
}

void epoll_loop::remove(handle fd) noexcept
{
    // NOTE: fails if the handle is already closed, which removes it anyway.
    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
}

// NOLINTNEXTLINE(readability-make-member-function-const): modifies the kernel object.
void epoll_loop::arm(handle fd, poll_op op)
{
    epoll_event ev{.events = to_epoll_events(op), .data = {.fd = fd}};
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) throw system_error_from_errno(errno, "failed to arm handle");
}

// NOLINTNEXTLINE(readability-make-member-function-const): modifies the kernel object.
std::size_t epoll_loop::wait(std::span<ready_event> events, std::optional<std::chrono::nanoseconds> timeout)
{
    std::array<epoll_event, 64> epoll_events{};
    const auto                  max_events = std::min(epoll_events.size(), events.size());

    // epoll_wait() only takes milliseconds: round up, so we don't wake up just before a deadline and spin.
    int timeout_ms = -1;
    if (timeout)
    {
        auto ms    = std::chrono::ceil<std::chrono::milliseconds>(*timeout).count();
        timeout_ms = static_cast<int>(std::clamp<decltype(ms)>(ms, 0, INT_MAX));
    }

    auto count = ::epoll_wait(epoll_fd, epoll_events.data(), static_cast<int>(max_events), timeout_ms);
    if (count == -1)
    {
        if (errno == EINTR) return 0;
        throw system_error_from_errno(errno, "failed to wait for epoll events");
    }

    std::size_t num_ready = 0;

    for (const auto& ev : std::span{epoll_events}.first(static_cast<std::size_t>(count)))
    {
        if (ev.data.fd == interrupt_fd)
        {
            std::uint64_t value = 0;
            [[maybe_unused]] auto _ = ::read(interrupt_fd, &value, sizeof(value));
            continue;
        }

        events[num_ready++] = {.fd = ev.data.fd, .ready = from_epoll_events(ev.events)};
    }

    return num_ready;
}

// NOLINTNEXTLINE(readability-make-member-function-const): modifies the kernel object.
void epoll_loop::interrupt() noexcept
{
    std::uint64_t value = 1;

    // NOTE: can only fail if the counter would overflow, in which case an interrupt is already pending.
    [[maybe_unused]] auto _ = ::write(interrupt_fd, &value, sizeof(value));
}

}

#endif
