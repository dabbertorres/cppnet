#include "io/aio/detail/kqueue_loop.hpp"

#include "config.hpp"

#ifdef NET_HAS_KQUEUE

// TODO: this implementation uses some OSX-specific features/flags/etc, so this should be
//       split out to support "regular" BSDs.

#    include <array>
#    include <atomic>
#    include <chrono>
#    include <coroutine>
#    include <cstdint>
#    include <mutex>
#    include <system_error>

#    include <unistd.h>

#    include <spdlog/spdlog.h>
#    include <sys/event.h>
#    include <sys/types.h>

#    include "exception.hpp"

#    include "io/aio/event.hpp"
#    include "io/aio/poll.hpp"

namespace net::io::aio::detail
{

kqueue_loop::kqueue_loop()
    : descriptor{kqueue()}
    , timeout_id{1}
{
    if (descriptor == -1) throw system_error_from_errno(errno, "failed to create kqueue");
}

kqueue_loop::~kqueue_loop() { shutdown(); }

void kqueue_loop::queue(const wait_for& trigger)
{
    // we may be adding/updating up to two events (for trigger, and for timeout)
    int num_new_events = 1;

    // clang-format off
    std::array<struct kevent, 2> new_events = {{
        {
            .ident  = static_cast<uintptr_t>(trigger.fd),
            .filter = static_cast<int16_t>(
                (readable(trigger.op) ? EVFILT_READ : 0) 
                | (writable(trigger.op) ? EVFILT_WRITE : 0)
            ),
            .flags  = EV_ADD | EV_DISPATCH | EV_ONESHOT | EV_CLEAR | EV_EOF,
            .fflags = 0,
            .data   = 0,
            .udata  = trigger.handle.address(),
        },
        {},
    }};
    // clang-format on

    if (trigger.timeout > decltype(trigger.timeout)::zero())
    {
        auto expires_at = trigger.timeout + clock::now();
        if (expires_at < clock::now()) expires_at = clock::now();

        new_events[1] = {
            .ident  = timeout_id.fetch_add(1, std::memory_order_relaxed),
            .filter = EVFILT_TIMER,
            .flags  = EV_ADD | EV_ONESHOT,
            .fflags = NOTE_ABSOLUTE | NOTE_CRITICAL,
            .data   = expires_at.time_since_epoch().count(),
            .udata  = trigger.handle.address(),
        };
        ++num_new_events;
    }

    auto res = kevent(descriptor, new_events.data(), num_new_events, nullptr, 0, nullptr);
    if (res == -1)
    {
        // TODO: throw/return an error?
        spdlog::error("error queueing {} new events: {}", num_new_events, errno);
        return;
    }
}

coro::generator<event> kqueue_loop::dispatch() const
{
    std::array<struct kevent, 16> events{};

    auto num_events = kevent(descriptor, nullptr, 0, events.data(), events.size(), nullptr);
    if (num_events == -1)
    {
        // TODO: throw/return an error?
        spdlog::error("error queueing reading new events: {}", errno);
    }
    else
    {
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
}

void kqueue_loop::shutdown() noexcept
{
    std::lock_guard lock{mutex};

    // TODO: anything else?

    if (descriptor != -1) ::close(descriptor);
    descriptor = -1;
}

}

#endif
