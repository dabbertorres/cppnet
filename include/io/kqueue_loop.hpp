#pragma once

#include "config.hpp" // IWYU pragma: keep

#ifdef NET_HAS_KQUEUE

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

// kqueue_loop implements EventLoop with kqueue: one-shot, level-triggered read and write filters per handle.
class kqueue_loop
{
public:
    kqueue_loop();

    kqueue_loop(const kqueue_loop&)            = delete;
    kqueue_loop& operator=(const kqueue_loop&) = delete;

    kqueue_loop(kqueue_loop&&)            = delete;
    kqueue_loop& operator=(kqueue_loop&&) = delete;

    ~kqueue_loop() noexcept;

    void add(handle fd);
    void remove(handle fd) noexcept;
    void arm(handle fd, poll_op op);

    std::size_t wait(std::span<ready_event> events, std::optional<std::chrono::nanoseconds> timeout);

    void interrupt() noexcept;

private:
    int descriptor;
};

}

#endif
