#include "config.hpp"

#ifdef NET_HAS_EPOLL

#    include "io/epoll_loop.hpp"

// keep parent header above this

#    include <array>
#    include <atomic>
#    include <cerrno>
#    include <chrono>
#    include <coroutine>
#    include <cstddef>
#    include <cstdint>
#    include <cstring>
#    include <memory>
#    include <stdexcept>
#    include <string>
#    include <type_traits>
#    include <utility>

#    include <sys/epoll.h>
#    include <sys/eventfd.h>
#    include <sys/timerfd.h>
#    include <unistd.h>

#    include <spdlog/logger.h>

#    include "coro/generator.hpp"
#    include "coro/task.hpp"
#    include "exception.hpp"
#    include "io/event.hpp"
#    include "io/io.hpp"
#    include "io/poll.hpp"

namespace
{

EPOLL_EVENTS convert_poll_op(net::io::poll_op op)
{
    using net::io::poll_op;

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

namespace net::io::detail
{

epoll_loop::epoll_loop(std::shared_ptr<spdlog::logger> logger)
    : epoll_fd{epoll_create1(EPOLL_CLOEXEC)}
    , schedule_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
    , timer_fd(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK))
    , shutdown_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
    , running{true}
    , logger{logger->clone("epoll_loop")}
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
    shutdown();

    if (epoll_fd != -1) ::close(epoll_fd);
    if (schedule_fd != -1) ::close(schedule_fd);
    if (timer_fd != -1) ::close(timer_fd);
    if (shutdown_fd != -1) ::close(shutdown_fd);
}

coro::task<result> epoll_loop::queue(io_handle handle, poll_op op, std::chrono::milliseconds timeout)
{
    auto r = co_await operation{this, handle, op, timeout};
    co_return r;
}

coro::generator<event> epoll_loop::dispatch() const
{
    std::array<epoll_event, 16> events;

    auto count = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), -1);

    for (auto i = 0u; i < static_cast<std::size_t>(count); ++i)
    {
        epoll_event& event      = events[i];
        auto*        handle_ptr = event.data.ptr;

        if (handle_ptr == schedule_ptr)
        {
            on_scheduled();
        }
        else if (handle_ptr == timer_ptr)
        {
            // TODO: process timeouts
            on_timeout();
        }
        else if (handle_ptr == shutdown_ptr) [[unlikely]]
        {
            // noop
        }
        else
        {
            // TODO: process an actual event
            on_event();
        }
    }
}

/*void epoll_loop::clear_schedule() noexcept*/
/*{*/
/*    eventfd_t value = 0;*/
/*    eventfd_read(schedule_fd, &value);*/
/*}*/

void epoll_loop::shutdown() noexcept
{
    running.store(false, std::memory_order::release);
    std::uint64_t value = 1;
    ::write(shutdown_fd, &value, sizeof(value));
}

void epoll_loop::operation::await_suspend(std::coroutine_handle<promise> await_on) noexcept
{
    awaiting = await_on;
    loop->queue(awaiting, handle, op, timeout);
}

result epoll_loop::operation::await_resume() noexcept { return awaiting.promise().result(); }

void epoll_loop::queue(std::coroutine_handle<promise> awaiting,
                       io_handle                      handle,
                       poll_op                        op,
                       std::chrono::milliseconds      timeout)
{
    epoll_event event{
        .events = convert_poll_op(op) | EPOLLONESHOT | EPOLLRDHUP | EPOLLET,
        .data   = {
                   .ptr = awaiting.address(),
                   }
    };

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, handle, &event) == -1) throw system_error_from_errno(errno);

    // we may be adding/updating up to three events (read, write, timeout)
    std::array<struct kevent, 3> new_events;

    auto num_new_events = 0u;

    if (is_readable(op)) new_events[num_new_events++] = make_io_kevent(awaiting, handle, EVFILT_READ);
    if (is_writable(op)) new_events[num_new_events++] = make_io_kevent(awaiting, handle, EVFILT_WRITE);
    if (is_set(timeout)) new_events[num_new_events++] = make_timeout_kevent(awaiting, timeout);

    if (descriptor == -1) return;

    logger->trace("[c {}]: queuing {} events for op {} on {}",
                  awaiting.address(),
                  num_new_events,
                  std::to_underlying(op),
                  handle);

    auto res = kevent(descriptor, new_events.data(), static_cast<int>(num_new_events), nullptr, 0, nullptr);
    if (res == -1)
    {
        auto err = errno;

        std::string errbuf(256, 0);
        auto        err_str = ::strerror_r(err, errbuf.data(), errbuf.length());

        // TODO: throw/return an error?
        logger->error("[c {}]: error queueing {} new events: {} ({})",
                      awaiting.address(),
                      num_new_events,
                      err_str,
                      err);
        return;
    }
}

}

#endif
