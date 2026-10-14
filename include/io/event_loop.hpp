#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>

#include "config.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"

namespace net::io
{

// ready_event reports that a handle is ready for some operations.
struct ready_event
{
    handle  fd;
    poll_op ready;
};

}

#ifdef NET_HAS_EPOLL
#    include "epoll_loop.hpp"

namespace net::io::detail
{

using event_loop = epoll_loop;

}

#elifdef NET_HAS_KQUEUE
#    include "kqueue_loop.hpp"

namespace net::io::detail
{

using event_loop = kqueue_loop;

}

#elifdef NET_HAS_COMPLETION_PORTS
#    include "completion_ports_loop.hpp"

namespace net::io::detail
{

using event_loop = completion_ports_loop;

}

#else
#    error "No supported event loop implementation."
#endif

namespace net::io
{

// EventLoop is the OS-specific part of the scheduler: it reports when handles are ready, and nothing else.
//
// - add/remove start and stop tracking a handle.
// - arm requests a single notification for the given operations (replacing any earlier request for the handle). Once
//   a handle has been reported ready, it must be armed again for further notifications. poll_op::none disarms it.
// - wait blocks until at least one handle is ready, the timeout passes, or interrupt() is called (from any thread),
//   and returns how many events were written. It may return spurious events (and 0 events).
//
// All but wait() may be called concurrently with wait().
// clang-format off
template<typename T>
concept EventLoop = std::is_default_constructible_v<T>
    && !std::is_copy_constructible_v<T>
    && !std::is_copy_assignable_v<T>
    && !std::is_move_constructible_v<T>
    && !std::is_move_assignable_v<T>
    && std::is_destructible_v<T>
    && requires(T& t, handle fd, poll_op op, std::span<ready_event> events, std::optional<std::chrono::nanoseconds> timeout)
    {
        { t.add(fd) } -> std::same_as<void>;
        { t.remove(fd) } noexcept -> std::same_as<void>;
        { t.arm(fd, op) } -> std::same_as<void>;
        { t.wait(events, timeout) } -> std::same_as<std::size_t>;
        { t.interrupt() } noexcept -> std::same_as<void>;
    };
// clang-format on

static_assert(EventLoop<detail::event_loop>);

}
