#pragma once

#include "config.hpp"

#if NET_HAS_EPOLL
#    include "detail/epoll_loop.hpp"
#elif NET_HAS_KQUEUE
#    include "detail/kqueue_loop.hpp"
#elif NET_HAS_COMPLETION_PORTS
#    include "detail/completion_ports_loop.hpp"
#else
#    error "No supported event loop implementation."
#endif
