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

#    include <unistd.h>

#    include <spdlog/spdlog.h>
#    include <sys/event.h>
#    include <sys/types.h>

#    include "exception.hpp"

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
            .ident  = timeout_id.fetch_add(1, std::memory_order_acq_rel),
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

void kqueue_loop::shutdown() noexcept
{
    std::lock_guard lock{mutex};

    // TODO: anything else?

    if (descriptor != -1) ::close(descriptor);
    descriptor = -1;
}

}

#endif
