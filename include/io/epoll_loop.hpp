#pragma once

#include "config.hpp" // IWYU pragma: keep

#ifdef NET_HAS_EPOLL

#    include <chrono>
#    include <cstddef>
#    include <optional>
#    include <span>

#    include "io/io.hpp"
#    include "io/poll.hpp"

namespace net::io
{

struct ready_event;

}

namespace net::io::detail
{

// epoll_loop implements EventLoop with epoll: one edge-triggered (EPOLLET) registration per handle, for reads and
// writes.
class epoll_loop
{
public:
    epoll_loop();

    epoll_loop(const epoll_loop&)            = delete;
    epoll_loop& operator=(const epoll_loop&) = delete;

    epoll_loop(epoll_loop&&)            = delete;
    epoll_loop& operator=(epoll_loop&&) = delete;

    ~epoll_loop() noexcept;

    void add(handle fd);
    void remove(handle fd) noexcept;

    std::size_t wait(std::span<ready_event> events, std::optional<std::chrono::nanoseconds> timeout);

    void interrupt() noexcept;

private:
    int epoll_fd;
    int interrupt_fd;
};

}

#endif
