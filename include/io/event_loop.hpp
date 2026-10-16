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
// - add/remove start and stop tracking a handle, for reads and writes.
// - wait blocks until at least one handle has become ready, the timeout passes, or interrupt() is called (from any
//   thread), and returns how many events were written. Handles are edge-triggered: one is reported when it becomes
//   ready (e.g. data arrives), not again for as long as it stays ready, so a caller must try its read or write (until
//   it would block) before waiting. It may return spurious events (and 0 events).
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
    && requires(T& t, handle fd, std::span<ready_event> events, std::optional<std::chrono::nanoseconds> timeout)
    {
        { t.add(fd) } -> std::same_as<void>;
        { t.remove(fd) } noexcept -> std::same_as<void>;
        { t.wait(events, timeout) } -> std::same_as<std::size_t>;
        { t.interrupt() } noexcept -> std::same_as<void>;
    };
// clang-format on

static_assert(EventLoop<detail::event_loop>);

}
