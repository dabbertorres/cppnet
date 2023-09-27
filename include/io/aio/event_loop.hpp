#pragma once

#include <chrono>
#include <concepts>
#include <type_traits>

#include "config.hpp"

#include "io/aio/poll.hpp"

namespace net::io::aio::detail
{

// clang-format off
template<typename T>
concept EventLoop = std::is_default_constructible_v<T>
    && !std::is_copy_constructible_v<T>
    && !std::is_copy_assignable_v<T>
    && !std::is_move_constructible_v<T>
    && !std::is_move_assignable_v<T>
    && std::is_destructible_v<T>
    && requires(T* t, std::chrono::milliseconds timeout)
    {
        { t->queue(wait_for{}, timeout) } -> std::same_as<void>;
        { t->dispatch() }                 -> std::same_as<void>;
        { t->shutdown() }                 -> std::same_as<void>;
    };
// clang-format on

}

#ifdef NET_HAS_EPOLL

#    include "detail/epoll_loop.hpp"

namespace net::io::aio
{

using event_loop = detail::epoll_loop;

static_assert(detail::EventLoop<event_loop>);

}

#elifdef NET_HAS_KQUEUE

#    include "detail/kqueue_loop.hpp"

namespace net::io::aio
{

using event_loop = detail::kqueue_loop;

static_assert(detail::EventLoop<event_loop>);

}

#elifdef NET_HAS_COMPLETION_PORTS

#    include "detail/completion_ports_loop.hpp"

namespace net::io::aio
{

using event_loop = detail::iocp_loop;

static_assert(detail::EventLoop<event_loop>);

}

#else

#    error "No supported event loop implementation."

#endif
