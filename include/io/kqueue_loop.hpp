#pragma once

#include "config.hpp" // IWYU pragma: keep

#ifdef NET_HAS_KQUEUE

#    include <chrono>
#    include <cstddef>
#    include <cstdint>
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

// kqueue_loop implements EventLoop with kqueue: edge-triggered (EV_CLEAR) read and write filters per handle.
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

    std::size_t wait(std::span<ready_event> events, std::optional<std::chrono::nanoseconds> timeout);

    void interrupt() noexcept;

private:
    // change applies flags (e.g. EV_ADD) to both of the handle's filters.
    void change(handle fd, std::uint16_t flags);

    handle descriptor;
};

}

#endif
