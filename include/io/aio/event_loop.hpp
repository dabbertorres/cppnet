#pragma once

#include <type_traits>

#include "config.hpp"

#include "io/aio/poll.hpp"

#ifdef NET_HAS_EPOLL
#    include "detail/epoll_loop.hpp"
#elifdef NET_HAS_KQUEUE
#    include "detail/kqueue_loop.hpp"
#elifdef NET_HAS_COMPLETION_PORTS
#    include "detail/completion_ports_loop.hpp"
#else
#    error "No supported event loop implementation."
#endif

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
    && requires(T* t, wait_for job)
    {
        { t->queue(job) } -> std::same_as<void>;
        { t->dispatch([](auto, auto) {}) } -> std::same_as<void>;
        { t->shutdown() } -> std::same_as<void>;
    };
// clang-format on

static_assert(EventLoop<event_loop>);

}

namespace net::io::aio
{

using event_loop = detail::event_loop;

}
