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
#    include <mutex>
#    include <stdexcept>
#    include <string>
#    include <system_error>
#    include <type_traits>

#    include <sys/epoll.h>
#    include <sys/eventfd.h>
#    include <sys/ioctl.h>
#    include <sys/time.h>
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

std::size_t roughly_get_socket_buffer_size(int fd, net::io::poll_op op)
{
    using net::io::poll_op;

    std::size_t request = 0;

    if (is_readable(op)) request = FIONREAD;
    else if (is_writable(op)) request = TIOCOUTQ;
    else return 0;

    int  value  = 0;
    auto status = ioctl(fd, request, &value);
    if (status == -1) return 0;

    return static_cast<std::size_t>(value);
}

}

namespace net::io::detail
{

epoll_loop::epoll_loop(const std::shared_ptr<spdlog::logger>& logger)
    : epoll_fd{epoll_create1(EPOLL_CLOEXEC)}
    , timer_fd(timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK))
    , shutdown_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK))
    , running{true}
    , logger{logger->clone("epoll_loop")}
{
    if (epoll_fd == -1) throw system_error_from_errno(errno, "epoll fd");
    if (timer_fd == -1) throw system_error_from_errno(errno, "timer fd");
    if (shutdown_fd == -1) throw system_error_from_errno(errno, "shutdown fd");

    epoll_event ev{.events = EPOLLIN};

    ev.data.fd = timer_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev) == -1) throw system_error_from_errno(errno, "add timer fd");

    ev.data.fd = shutdown_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, shutdown_fd, &ev) == -1)
        throw system_error_from_errno(errno, "add shutdown fd");
}

epoll_loop::~epoll_loop()
{
    shutdown();

    if (epoll_fd != -1) ::close(epoll_fd);
    if (timer_fd != -1) ::close(timer_fd);
    if (shutdown_fd != -1) ::close(shutdown_fd);
}

// NOLINTNEXTLINE(readability-make-member-function-const, "not really const")
void epoll_loop::register_handle(io_handle handle)
{
    epoll_event event{.events = 0, .data = {nullptr}};

    auto status = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, handle, &event);
    if (status == -1)
    {
        auto err = errno;
        if (err != EEXIST) // ignore already added fds
            throw system_error_from_errno(err, "registering handle");
    }
}

// NOLINTNEXTLINE(readability-make-member-function-const, "not really const")
void epoll_loop::deregister_handle(io_handle handle)
{
    epoll_event ev{.events = 0, .data = {nullptr}};

    auto status = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, handle, &ev);
    if (status == -1)
    {
        auto err = errno;
        if (err != ENOENT) // ignore already gone fds
            throw system_error_from_errno(err, "deregister_handle()");
    }

    // TODO: remove from timeout_list and ready_fds if needed
}

coro::task<result> epoll_loop::queue(io_handle handle, poll_op op, std::chrono::milliseconds timeout)
{
    auto r = co_await operation{this, handle, op, timeout};
    co_return r;
}

coro::generator<event> epoll_loop::dispatch()
{
    if (!running.load(std::memory_order::acquire)) co_return;

    std::array<epoll_event, 16> events{};

    auto count = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), -1);
    if (count == -1) throw system_error_from_errno(errno, "epoll_wait");

    bool dispatch_timeouts = false;

    for (auto i = 0u; i < static_cast<std::size_t>(count); ++i)
    {
        epoll_event& ev = events[i];

        if (ev.data.ptr == &timer_fd)
        {
            dispatch_timeouts = true;
            continue;
        }

        if (ev.data.ptr == &shutdown_fd) continue;

        auto handle = std::coroutine_handle<promise>::from_address(ev.data.ptr);

        // NOTE: epoll doesn't tell us how much data is available. We can get it,
        // but we don't store the fd on the event.
        //
        // So how do we resolve this? Use the language's coroutine machinerys! Where is that?
        // On the awaitable! (operation). Specifically, in operation::await_resume();
        io::result res = {};
        if ((ev.events & EPOLLERR) == EPOLLERR) res.err = std::make_error_condition(static_cast<std::errc>(errno));
        else if ((ev.events & EPOLLRDHUP) == EPOLLRDHUP) res.err = make_error_condition(status_condition::closed);
        else if ((ev.events & EPOLLHUP) == EPOLLHUP) res.err = make_error_condition(status_condition::closed);

        co_yield event{handle, res};
    }

    if (dispatch_timeouts)
    {
        std::lock_guard lock{timeout_list_mu};

        auto now = clock::now();

        while (!timeout_list.empty() && timeout_list.top().timeout_at <= now)
        {
            auto op = timeout_list.top();
            timeout_list.pop();

            if (!op.handle || op.handle.done()) continue;

            io::result res = {
                .count = 0,
                .err   = make_error_condition(status_condition::timed_out),
            };
            co_yield event{op.handle, res};
        }

        if (!timeout_list.empty())
        {
            auto next_timeout = timeout_list.top().timeout_at - clock::now();
            update_timer(std::chrono::duration_cast<std::chrono::milliseconds>(next_timeout));
        }
    }
}

void epoll_loop::shutdown() noexcept
{
    running.store(false, std::memory_order::release);
    std::uint64_t value = 1;
    ::write(shutdown_fd, &value, sizeof(value));
}

bool epoll_loop::operation::await_ready() const noexcept
{
    auto count = roughly_get_socket_buffer_size(handle, op);
    return count > 0;
}

result epoll_loop::operation::await_resume() noexcept
{
    auto res = awaiting.promise().result();

    auto count = roughly_get_socket_buffer_size(handle, op);
    if (count > 0) res.count = count;

    return res;
}

void epoll_loop::operation::await_suspend(std::coroutine_handle<promise> await_on)
{
    awaiting = await_on;
    loop->queue(awaiting, handle, op, timeout);
}

void epoll_loop::queue(std::coroutine_handle<promise> awaiting,
                       io_handle                      handle,
                       poll_op                        op,
                       std::chrono::milliseconds      timeout)
{
    epoll_event ev{.events = convert_poll_op(op) | EPOLLONESHOT | EPOLLRDHUP};
    ev.data.ptr = awaiting.address();

    auto status = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, handle, &ev);
    if (status == -1) throw system_error_from_errno(errno);

    // If this timeout is shorter than the current (if any), check if we need to update the timer.
    if (timeout > decltype(timeout)::zero())
    {
        std::lock_guard lock{timeout_list_mu};

        auto timeout_at = timeout + clock::now();

        if (timeout_list.empty() || timeout_list.top().timeout_at > timeout_at)
        {
            update_timer(timeout);
        }

        timeout_list.push(timeout_operation{
            .handle     = awaiting,
            .timeout_at = timeout_at,
        });
    }
}

// NOLINTNEXTLINE(readability-make-member-function-const, "not really const")
void epoll_loop::update_timer(std::chrono::milliseconds timeout)
{
    auto sec  = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout) - sec;

    itimerspec timeout_at = {
        .it_interval = {          .tv_sec = 0,            .tv_nsec = 0},
        .it_value    = {.tv_sec = sec.count(), .tv_nsec = nsec.count()},
    };
    auto status = timerfd_settime(timer_fd, 0, &timeout_at, nullptr);
    if (status == -1) throw system_error_from_errno(errno, "updating timer");
}

}

#endif
