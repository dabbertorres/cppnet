#include "config.hpp" // IWYU pragma: keep

#ifdef NET_HAS_KQUEUE

#    include "io/kqueue_loop.hpp"

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
#    include <ctime>
#    include <memory>
#    include <string>
#    include <system_error>
#    include <utility>

#    include <sys/event.h>
#    include <sys/types.h>
#    include <unistd.h>

#    include <spdlog/common.h>
#    include <spdlog/logger.h>
#    include <spdlog/spdlog.h>

#    include "coro/generator.hpp"
#    include "coro/task.hpp"
#    include "exception.hpp"
#    include "io/event.hpp"
#    include "io/io.hpp"
#    include "io/poll.hpp"

namespace
{

constexpr bool is_set(std::chrono::milliseconds v) noexcept { return v > decltype(v)::zero(); }

}

namespace net::io::detail
{

kqueue_loop::kqueue_loop(std::shared_ptr<spdlog::logger> logger)
    : running{true}
    , descriptor{kqueue()}
    , timeout_id{1}
    , logger{logger->clone("kqueue_loop")}
{
    if (descriptor == -1) throw system_error_from_errno(errno, "failed to create kqueue");
}

kqueue_loop::~kqueue_loop()
{
    shutdown();

    if (descriptor != -1)
    {
        // TODO: anything else?
        ::close(descriptor);
    }
}

coro::task<result> kqueue_loop::queue(io_handle handle, poll_op op, std::chrono::milliseconds timeout)
{
    auto r = co_await operation{this, handle, op, timeout};
    co_return r;
}

coro::generator<event> kqueue_loop::dispatch() const
{
    std::array<struct kevent, 16> events;

    if (descriptor == -1) co_return;

    /*struct timespec timeout*/
    /*{*/
    /*    .tv_sec = 0, .tv_nsec = 1000000*/
    /*};*/

    auto num_events = kevent(descriptor, nullptr, 0, events.data(), events.size(), nullptr); //&timeout);
    if (num_events == -1)
    {
        auto err = errno;

        /*if (err == EBADF || err == EINTR)*/
        /*{*/
        /*    // probably due to shutting down...*/
        /*    co_return;*/
        /*}*/

        std::string errbuf(256, 0);
        int         length = ::strerror_r(err, errbuf.data(), errbuf.length());
        errbuf.resize(static_cast<std::size_t>(length));

        // TODO: throw/return an error?
        logger->error("error reading new events: {} ({})", errbuf, err);
        co_return;
    }

    // if we got no events and we're supposed to shutdown, we're done
    if (num_events == 0 && !running.load(std::memory_order::acquire)) co_return;

    for (auto i = 0u; i < static_cast<std::size_t>(num_events); ++i)
    {
        auto& ev     = events[i];
        auto  handle = std::coroutine_handle<promise>::from_address(ev.udata);

        logger->trace("[c {}]: got event for {}: {} of size {}", handle.address(), ev.ident, ev.filter, ev.data);

        // don't try to invoke coroutines that have already finished.
        if (handle.done())
        {
            logger->trace("[c {}]: coroutine for {} {} event already finished", handle.address(), ev.ident, ev.filter);
            break;
        }

        result res = {
            .count = static_cast<std::size_t>(ev.data),
            .err   = {},
        };

        if ((ev.flags & EV_ERROR) != 0)
        {
            res.err = std::make_error_condition(static_cast<std::errc>(ev.data));
        }
        else
        {
            switch (ev.filter)
            {
            case EVFILT_TIMER: res.err = make_error_condition(status_condition::timed_out); break;

            case EVFILT_READ: [[fallthrough]];
            case EVFILT_WRITE:
                if ((ev.flags & EV_EOF) != 0)
                {
                    // Did it shutdown due to an error? Or did the socket just close?
                    res.err = ev.fflags != 0 ? std::make_error_condition(static_cast<std::errc>(ev.fflags))
                                             : make_error_condition(status_condition::closed);
                }

                break;

            default:
                logger->warn("[c {}]: unexpected event type {} for {}", handle.address(), ev.filter, ev.ident);
                break;
            }
        }

        logger->trace("[c {}]: yielding event for {}; error = '{}'", handle.address(), ev.ident, res.err.message());

        co_yield event{handle, res};
    }
}

void kqueue_loop::shutdown() noexcept { running.store(false, std::memory_order::release); }

void kqueue_loop::operation::await_suspend(std::coroutine_handle<promise> await_on) noexcept
{
    awaiting = await_on;
    loop->queue(awaiting, handle, op, timeout);
}

result kqueue_loop::operation::await_resume() noexcept { return awaiting.promise().result(); }

void kqueue_loop::queue(std::coroutine_handle<promise> awaiting,
                        io_handle                      handle,
                        poll_op                        op,
                        std::chrono::milliseconds      timeout)
{
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
        int         length = ::strerror_r(err, errbuf.data(), errbuf.length());
        errbuf.resize(static_cast<std::size_t>(length));

        // TODO: throw/return an error?
        logger->error("[c {}]: error queueing {} new events: {} ({})", awaiting.address(), num_new_events, errbuf, err);
        return;
    }
}

struct kevent
kqueue_loop::make_io_kevent(std::coroutine_handle<promise> awaiting, io_handle handle, int16_t filter) const noexcept
{
    return {
        .ident  = static_cast<uintptr_t>(handle),
        .filter = filter,
        .flags  = EV_ADD | EV_DISPATCH | EV_ONESHOT | EV_CLEAR | EV_EOF,
        .fflags = 0,
        .data   = 0,
        .udata  = awaiting.address(),
    };
}

struct kevent kqueue_loop::make_timeout_kevent(std::coroutine_handle<promise> awaiting,
                                               std::chrono::milliseconds      timeout) noexcept
{
    auto expires_at = timeout + clock::now();
    if (expires_at < clock::now()) expires_at = clock::now();

    return {
        .ident  = timeout_id.fetch_add(1, std::memory_order_relaxed),
        .filter = EVFILT_TIMER,
        .flags  = EV_ADD | EV_ONESHOT,
        .fflags = NOTE_ABSOLUTE | NOTE_CRITICAL,
        .data   = expires_at.time_since_epoch().count(),
        .udata  = awaiting.address(),
    };
}

}

#endif
