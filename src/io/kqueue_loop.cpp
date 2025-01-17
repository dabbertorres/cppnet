#include "config.hpp"

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
#    include <tuple>

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

std::string get_errno_msg(int err)
{
    std::string errbuf(256, 0);
    int         length = ::strerror_r(err, errbuf.data(), errbuf.length());
    errbuf.resize(static_cast<std::size_t>(length));

    return errbuf;
}

}

namespace net::io::detail
{

kqueue_loop::kqueue_loop(const std::shared_ptr<spdlog::logger>& logger)
    : running{true}
    , descriptor{kqueue()}
    , timeout_id{1}
    , logger{logger->clone("kqueue_loop")}
{
    if (descriptor == -1) throw system_error_from_errno(errno, "failed to create kqueue");

    // also listen for shutdown signal

    struct kevent shutdown_event = {
        .ident  = shutdown_ident,
        .filter = EVFILT_USER,
        .flags  = EV_ADD | EV_DISPATCH | EV_ONESHOT | EV_CLEAR,
        .fflags = 0,
        .data   = 0,
        .udata  = nullptr,
    };

    auto res = kevent(descriptor, &shutdown_event, 1, nullptr, 0, nullptr);
    if (res == -1) throw system_error_from_errno(errno, "failed to register for user shutdown event");
}

kqueue_loop::~kqueue_loop() noexcept
{
    shutdown();

    if (descriptor != -1)
    {
        // TODO: anything else?
        ::close(descriptor);
    }
}

coro::task<result> kqueue_loop::queue(handle fd, poll_op op, std::chrono::milliseconds timeout)
{
    auto r = co_await operation{this, fd, op, timeout};
    co_return r;
}

coro::generator<event> kqueue_loop::dispatch()
{
    std::array<struct kevent, 16> events{};

    if (descriptor == -1 || !running.load(std::memory_order::acquire)) co_return;

    /*struct timespec timeout*/
    /*{*/
    /*    .tv_sec = 0, .tv_nsec = 1000000*/
    /*};*/

    auto num_events = kevent(descriptor, nullptr, 0, events.data(), events.size(), nullptr);
    if (num_events == -1)
    {
        auto err = errno;

        /*if (err == EBADF || err == EINTR)*/
        /*{*/
        /*    // probably due to shutting down...*/
        /*    co_return;*/
        /*}*/

        auto msg = get_errno_msg(err);

        // TODO: throw/return an error?
        logger->error("error reading new events: {} ({})", msg, err);
        co_return;
    }

    // if we got no events and we're supposed to shutdown, we're done
    if (num_events == 0 && !running.load(std::memory_order::acquire)) co_return;

    bool notify_shutdown = false;

    for (auto i = 0u; i < static_cast<std::size_t>(num_events); ++i)
    {
        auto& kev = events[i];
        if (kev.filter == EVFILT_USER)
        {
            switch (kev.ident)
            {
            case shutdown_ident:
                // shutdown signal - process any other events (if any) before returning.
                notify_shutdown = true;
                break;

            default: logger->warn("unexpected ident for EVFILT_USER: {}", kev.ident); break;
            }

            continue;
        }

        auto [ev, ok] = translate_kevent(kev);
        if (ok)
        {
            co_yield ev;
        }
    }

    if (notify_shutdown)
    {
        running.store(false, std::memory_order::release);
        running.notify_one();
    }
}

std::tuple<event, bool> kqueue_loop::translate_kevent(const struct kevent& ev) const noexcept
{
    auto handle = std::coroutine_handle<promise>::from_address(ev.udata);

    // don't try to invoke coroutines that have already finished.
    if (handle.done()) return {{}, false};

    result res;

    if ((ev.flags & EV_ERROR) != 0)
    {
        res.err = std::make_error_condition(static_cast<std::errc>(ev.data));
    }
    else
    {
        res.count = static_cast<std::size_t>(ev.data);

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

        default: logger->warn("[c {}]: unexpected event type {} for {}", handle.address(), ev.filter, ev.ident); break;
        }
    }

    return {
        {.handle = handle, .result = res},
        true
    };
}

void kqueue_loop::shutdown() noexcept
{
    if (!running.load(std::memory_order::acquire)) return;

    struct kevent trigger_shutdown = {
        .ident  = shutdown_ident,
        .filter = EVFILT_USER,
        .flags  = 0,
        .fflags = NOTE_TRIGGER,
        .data   = 0,
        .udata  = nullptr,
    };

    auto res = kevent(descriptor, &trigger_shutdown, 1, nullptr, 0, nullptr);
    if (res == -1)
    {
        auto err = errno;
        logger->error("error triggering shutdown event: {}: {}", err, get_errno_msg(err));
        running.store(false, std::memory_order::release);
        return;
    }

    running.wait(true, std::memory_order::acquire);
}

void kqueue_loop::operation::await_suspend(std::coroutine_handle<promise> await_on) noexcept
{
    awaiting = await_on;
    loop->queue(awaiting, fd, op, timeout);
}

result kqueue_loop::operation::await_resume() noexcept { return awaiting.promise().result(); }

void kqueue_loop::queue(std::coroutine_handle<promise> awaiting,
                        handle                         fd,
                        poll_op                        op,
                        std::chrono::milliseconds      timeout)
{
    // we may be adding/updating up to three events (read, write, timeout)
    std::array<struct kevent, 3> new_events{};

    auto num_new_events = 0u;

    if (is_readable(op)) new_events[num_new_events++] = make_io_kevent(awaiting, fd, EVFILT_READ);
    if (is_writable(op)) new_events[num_new_events++] = make_io_kevent(awaiting, fd, EVFILT_WRITE);
    if (is_set(timeout)) new_events[num_new_events++] = make_timeout_kevent(awaiting, timeout);

    if (descriptor == -1) return;

    auto res = kevent(descriptor, new_events.data(), static_cast<int>(num_new_events), nullptr, 0, nullptr);
    if (res == -1)
    {
        auto err = errno;
        auto msg = get_errno_msg(err);

        // TODO: throw/return an error?
        logger->error("[c {}]: error queueing {} new events: {} ({})", awaiting.address(), num_new_events, msg, err);
        return;
    }
}

struct kevent
kqueue_loop::make_io_kevent(std::coroutine_handle<promise> awaiting, handle fd, int16_t filter) const noexcept
{
    return {
        .ident  = static_cast<uintptr_t>(fd),
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
