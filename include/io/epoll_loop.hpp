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

// epoll_loop implements EventLoop with epoll: one level-triggered, one-shot registration per handle.
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
    void arm(handle fd, poll_op op);

    std::size_t wait(std::span<ready_event> events, std::optional<std::chrono::nanoseconds> timeout);

    void interrupt() noexcept;

private:
    int epoll_fd;
    int interrupt_fd;
};

}

#endif
