#include "io/kqueue_loop.hpp"

#include "coro/task.hpp"

#include "config.hpp" // IWYU pragma: keep

#ifdef NET_HAS_KQUEUE

// TODO: this implementation uses some OSX-specific features/flags/etc, so this should be
//       split out to support "regular" BSDs.

#    include <array>
#    include <atomic>
#    include <cerrno>
#    include <chrono>
#    include <coroutine>
#    include <cstddef>
#    include <cstdint>
#    include <cstring>
#    include <string>
#    include <system_error>

#    include <unistd.h>

#    include <spdlog/spdlog.h>
#    include <sys/event.h>
#    include <sys/types.h>

#    include "coro/generator.hpp"
#    include "io/event.hpp"
#    include "io/io.hpp"
#    include "io/poll.hpp"

#    include "exception.hpp"

namespace net::io::detail
{

void kqueue_loop::operation::await_suspend(std::coroutine_handle<promise> await_on) noexcept
{
    awaiting = await_on;
    loop->queue(awaiting, handle, op, timeout);
}

result kqueue_loop::operation::await_resume() noexcept { return awaiting.promise().result(); }

kqueue_loop::kqueue_loop()
    : descriptor{kqueue()}
    , timeout_id{1}
{
    if (descriptor == -1) throw system_error_from_errno(errno, "failed to create kqueue");
}

kqueue_loop::~kqueue_loop() { shutdown(); }

coro::task<result> kqueue_loop::queue(io_handle handle, poll_op op, std::chrono::milliseconds timeout)
{
    auto r = co_await operation{this, handle, op, timeout};
    co_return r;
}

void kqueue_loop::queue(std::coroutine_handle<promise> awaiting,
                        io_handle                      handle,
                        poll_op                        op,
                        std::chrono::milliseconds      timeout)
{
    // we may be adding/updating up to two events (for trigger, and for timeout)
    int num_new_events = 1;

    // clang-format off
    std::array<struct kevent, 2> new_events = {{
        {
            .ident  = static_cast<uintptr_t>(handle),
            .filter = static_cast<int16_t>(
                (readable(op) ? EVFILT_READ : 0) 
                | (writable(op) ? EVFILT_WRITE : 0)
            ),
            .flags  = EV_ADD | EV_DISPATCH | EV_ONESHOT | EV_CLEAR | EV_EOF,
            .fflags = 0,
            .data   = 0,
            .udata  = awaiting.address(),
        },
        {},
    }};
    // clang-format on

    if (timeout > decltype(timeout)::zero())
    {
        auto expires_at = timeout + clock::now();
        if (expires_at < clock::now()) expires_at = clock::now();

        new_events[1] = {
            .ident  = timeout_id.fetch_add(1, std::memory_order_relaxed),
            .filter = EVFILT_TIMER,
            .flags  = EV_ADD | EV_ONESHOT,
            .fflags = NOTE_ABSOLUTE | NOTE_CRITICAL,
            .data   = expires_at.time_since_epoch().count(),
            .udata  = awaiting.address(),
        };
        ++num_new_events;
    }

    auto fd = descriptor.load();
    if (fd == -1) return;

    auto res = kevent(fd, new_events.data(), num_new_events, nullptr, 0, nullptr);
    if (res == -1)
    {
        auto err = errno;

        std::string errbuf(256, 0);
        int         length = ::strerror_r(err, errbuf.data(), errbuf.length());
        errbuf.resize(static_cast<std::size_t>(length));

        // TODO: throw/return an error?
        spdlog::error("error queueing {} new events: {} ({})", num_new_events, errbuf, err);
        return;
    }
}

coro::generator<event> kqueue_loop::dispatch() const
{
    std::array<struct kevent, 16> events{};

    auto fd = descriptor.load();
    if (fd == -1) co_return;

    auto num_events = kevent(fd, nullptr, 0, events.data(), events.size(), nullptr);
    if (num_events == -1)
    {
        auto err = errno;

        if (err == EBADF || err == EINTR)
        {
            // probably due to shutting down...
            co_return;
        }

        std::string errbuf(256, 0);
        int         length = ::strerror_r(err, errbuf.data(), errbuf.length());
        errbuf.resize(static_cast<std::size_t>(length));

        // TODO: throw/return an error?
        spdlog::error("error queueing reading new events: {} ({})", errbuf, err);
        co_return;
    }

    for (auto i = 0u; i < static_cast<std::size_t>(num_events); ++i)
    {
        auto& ev = events[i];
        switch (ev.filter)
        {
        case EVFILT_TIMER:
        {
            auto handle = std::coroutine_handle<promise>::from_address(events[i].udata);

            // don't try to invoke coroutines that have already finished.
            if (handle.done()) break;

            result res = {
                .count = 0,
                .err   = make_error_condition(status_condition::timed_out),
            };

            co_yield event{handle, res};
            break;
        }

        case EVFILT_READ: [[fallthrough]];
        case EVFILT_WRITE:
        {
            auto handle = std::coroutine_handle<promise>::from_address(events[i].udata);

            // don't try to invoke coroutines that have already finished.
            if (handle.done()) break;

            result res = {
                .count = static_cast<std::size_t>(ev.data),
                .err   = {},
            };

            if ((ev.flags & EV_EOF) != 0)
            {
                // Did it shutdown due to an error? Or did the socket just close?
                res.err = ev.fflags != 0 ? std::make_error_condition(static_cast<std::errc>(ev.fflags))
                                         : make_error_condition(status_condition::closed);
            }

            co_yield event{handle, res};
            break;
        }

        default:
            // no-op
        }
    }
}

void kqueue_loop::shutdown() noexcept
{
    if (auto fd = descriptor.exchange(-1); fd != -1)
    {
        // TODO: anything else?
        ::close(fd);
    }
}

}

#endif
