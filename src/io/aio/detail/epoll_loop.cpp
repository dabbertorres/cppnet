#include <exception>
#include <stdexcept>
#include <string>

#include "config.hpp"

#ifdef NET_HAS_EPOLL

#    include <cstdint>

#    include <unistd.h>

#    include <sys/epoll.h>
#    include <sys/eventfd.h>
#    include <sys/timerfd.h>

#    include "exception.hpp"

#    include "io/aio/detail/epoll_loop.hpp"

namespace
{

EPOLL_EVENTS convert_poll_op(net::io::aio::poll_op op)
{
    using net::io::aio::poll_op;

    using namespace std::string_literals;

    switch (op)
    {
    case poll_op::read: return EPOLLIN;
    case poll_op::write: return EPOLLOUT;
    case poll_op::read_write:
        return static_cast<EPOLL_EVENTS>(EPOLLIN | EPOLLOUT);
    [[unlikely]] default:
        throw std::runtime_error("invalid poll_op value: "s
                                 + std::to_string(static_cast<std::underlying_type_t<poll_op>>(op)));
    }
}

}

namespace net::io::aio::detail
{

epoll_loop::epoll_loop()
    : epoll_fd{epoll_create1(EPOLL_CLOEXEC)}
    , schedule_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
    , timer_fd(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK))
    , shutdown_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
    , events{}
{
    if (epoll_fd == -1) throw system_error_from_errno(errno, "epoll fd");
    if (schedule_fd == -1) throw system_error_from_errno(errno, "schedule fd");
    if (timer_fd == -1) throw system_error_from_errno(errno, "timer fd");
    if (shutdown_fd == -1) throw system_error_from_errno(errno, "shutdown fd");

    epoll_event event{.events = EPOLLIN};

    event.data.ptr = const_cast<void*>(schedule_ptr); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, schedule_fd, &event) == -1)
        throw system_error_from_errno(errno, "add schedule fd");

    event.data.ptr = const_cast<void*>(timer_ptr); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &event) == -1)
        throw system_error_from_errno(errno, "add timer fd");

    event.data.ptr = const_cast<void*>(shutdown_ptr); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, shutdown_fd, &event) == -1)
        throw system_error_from_errno(errno, "add shutdown fd");
}

epoll_loop::~epoll_loop()
{
    if (epoll_fd != -1) ::close(epoll_fd);
    if (schedule_fd != -1) ::close(schedule_fd);
    if (timer_fd != -1) ::close(timer_fd);
    if (shutdown_fd != -1) ::close(shutdown_fd);
}

void epoll_loop::poll(int fd, poll_op op, std::chrono::milliseconds timeout)
{
    epoll_event event{.events = convert_poll_op(op) | EPOLLONESHOT | EPOLLRDHUP};

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1) throw system_error_from_errno(errno);
}

void epoll_loop::clear_schedule() noexcept
{
    eventfd_t value = 0;
    eventfd_read(schedule_fd, &value);
}

void epoll_loop::shutdown() noexcept
{
    std::uint64_t value = 1;
    ::write(shutdown_fd, &value, sizeof(value));
}

}

#endif
