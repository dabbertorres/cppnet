#pragma once

#include <chrono>
#include <concepts>
#include <type_traits>

#include "config.hpp"
#include "coro/generator.hpp"
#include "coro/task.hpp"
#include "io/event.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"

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

// clang-format off
template<typename T>
concept EventLoop = std::is_default_constructible_v<T>
    && !std::is_copy_constructible_v<T>
    && !std::is_copy_assignable_v<T>
    && !std::is_move_constructible_v<T>
    && !std::is_move_assignable_v<T>
    && std::is_destructible_v<T>
    && requires(T* t, io_handle handle, poll_op op, std::chrono::milliseconds timeout)
    {
        { t->queue(handle, op, timeout) } -> std::same_as<coro::task<result>>;
        { t->dispatch() } -> std::same_as<coro::generator<detail::event>>;
        { t->shutdown() } -> std::same_as<void>;
        { t->register_handle(handle) } -> std::same_as<void>;
        { t->deregister_handle(handle) } -> std::same_as<void>;
    };
// clang-format on

static_assert(EventLoop<detail::event_loop>);

using event_loop = detail::event_loop;

}
