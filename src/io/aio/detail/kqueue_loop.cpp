#include "io/aio/detail/kqueue_loop.hpp"

#include <array>
#include <chrono>
#include <coroutine>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

#include "config.hpp"
#include "exception.hpp"

#include "io/aio/poll.hpp"

#ifdef NET_HAS_KQUEUE

// TODO: this implementation uses some OSX-specific features/flags/etc, so this should be
//       split out to support "regular" BSDs.

#    include <cstring>

#    include <unistd.h>

#    include <sys/event.h>
#    include <sys/types.h>

namespace net::io::aio::detail
{

kqueue_loop::kqueue_loop()
    : descriptor(kqueue())
    , timeout_descriptor(7) // arbitrarily chosen
{
    if (descriptor == -1) throw system_error_from_errno(errno, "failed to create kqueue");
}

kqueue_loop::~kqueue_loop() { shutdown(); }

void kqueue_loop::queue(wait_for&& trigger, std::chrono::milliseconds timeout)
{
    // we may be adding/updating up to two events (for trigger, and for timeout)
    std::size_t num_new_events = 1;

    // clang-format off
    std::array<struct kevent, 2> new_events = {{
        {
            .ident  = static_cast<uintptr_t>(trigger.fd),
            .filter = 0,
            .flags  = EV_ADD | EV_DISPATCH | EV_ONESHOT | EV_CLEAR | EV_EOF,
            .fflags = 0,
            .data   = 0,
            .udata  = trigger.handle.address(),
        },
        {},
    }};
    // clang-format on

    if (readable(trigger.op)) new_events[0].filter = EVFILT_READ;
    else if (writable(trigger.op)) new_events[0].filter = EVFILT_WRITE;

    if (timeout > decltype(timeout)::zero())
    {
        if (auto opt = build_timeout_event(trigger.handle, timeout); opt.has_value())
        {
            new_events[1] = *opt;
            ++num_new_events;
        }
    }

    auto res = kevent(descriptor, new_events.data(), static_cast<int>(num_new_events), nullptr, 0, nullptr);
    if (res == -1)
    {
        // TODO: throw/return an error?
        spdlog::error("error queueing {} new events: {}", num_new_events, errno);
        return;
    }
}

void kqueue_loop::dispatch()
{
    std::array<struct kevent, 16> events{};

    auto num_events = kevent(descriptor, nullptr, 0, events.data(), events.size(), nullptr);
    if (num_events == -1)
    {
        // TODO: throw/return an error?
        spdlog::error("error queueing reading new events: {}", errno);
        return;
    }

    bool timeout_completed = false;

    for (auto i = 0u; i < static_cast<std::size_t>(num_events); ++i)
    {
        auto& ev = events[i];
        if (ev.filter == EVFILT_TIMER && ev.ident == timeout_descriptor)
        {
            // TODO: timeout-able coroutine_handles need a specific promise type that we
            //       can use here, and set a value indicating a timeout.
            auto handle = std::coroutine_handle<>::from_address(events[i].udata);
            if (!handle.done())
            {
                handle.resume();
            }

            // TODO: see above
            // handle.promise().set_value(I TIMED OUT);

            timeouts.pop();
            timeout_completed = true;
        }
        else
        {
            auto handle = std::coroutine_handle<>::from_address(events[i].udata);
            if (!handle.done())
            {
                // TODO: run on an executor (e.g. thread pool)
                handle.resume();
            }
        }
    }

    if (timeout_completed) queue_next_timeout();
}

/* void kqueue_loop::clear_schedule() noexcept */
/* { */
/*   */
/* } */

void kqueue_loop::shutdown() noexcept
{
    std::lock_guard lock{mutex};

    // TODO: anything else?

    if (descriptor != -1) ::close(descriptor);
    descriptor = -1;
}

void kqueue_loop::queue_next_timeout() noexcept
{
    while (!timeouts.empty())
    {
        auto next = timeouts.top();
        if (next.handle.done())
        {
            timeouts.pop();
            continue;
        }

        auto time_left = std::chrono::duration_cast<std::chrono::milliseconds>(next.expires_at - clock::now());

        // dispatch any expired timeouts
        if (time_left <= decltype(time_left)::zero())
        {
            // TODO:
            // next.handle.set_value(I TIMED OUT);
            next.handle.resume();
            timeouts.pop();
            continue;
        }

        if (auto opt = build_timeout_event(next.handle, time_left); opt.has_value())
        {
            auto event = *opt;
            auto res   = kevent(descriptor, &event, 1, nullptr, 0, nullptr);
            if (res == -1)
            {
                // TODO: throw a system_error of some sort?
                spdlog::error("error re-queueing timeout event: {}", errno);
            }

            // found the next one!
            return;
        }
    }
}

std::optional<struct kevent> kqueue_loop::build_timeout_event(std::coroutine_handle<>   handle,
                                                              std::chrono::milliseconds timeout) noexcept
{
    auto expires_at = timeout + clock::now();
    if (expires_at < clock::now()) expires_at = clock::now();

    auto prev_next = timeouts.top();
    timeouts.emplace(handle, expires_at);
    if (timeouts.top() != prev_next)
    {
        // next timeout changed - we need to update the timer event
        return std::make_optional<struct kevent>({
            .ident  = timeout_descriptor,
            .filter = EVFILT_TIMER,
            .flags  = EV_ADD | EV_ONESHOT,
            .fflags = NOTE_ABSOLUTE,
            .data   = expires_at.time_since_epoch().count(),
            .udata  = handle.address(),
        });
    }

    return std::nullopt;
}

}

#endif
