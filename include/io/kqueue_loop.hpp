#pragma once

#include "config.hpp" // IWYU pragma: keep

#ifdef NET_HAS_KQUEUE

#    include <atomic>
#    include <chrono>
#    include <coroutine>
#    include <ctime>
#    include <mutex>

#    include <spdlog/spdlog.h>
#    include <sys/event.h>
#    include <sys/types.h>

#    include "coro/generator.hpp"
#    include "io/event.hpp"
#    include "io/poll.hpp"

namespace net::io::detail
{

using namespace std::chrono_literals;

class kqueue_loop
{
public:
    kqueue_loop();

    kqueue_loop(const kqueue_loop&)            = delete;
    kqueue_loop& operator=(const kqueue_loop&) = delete;

    kqueue_loop(kqueue_loop&&) noexcept            = delete;
    kqueue_loop& operator=(kqueue_loop&&) noexcept = delete;

    ~kqueue_loop();

    void queue(std::coroutine_handle<> handle, int fd);
    void queue(const wait_for& trigger);

    coro::generator<event> dispatch() const;

    void shutdown() noexcept;

private:
    using clock = std::chrono::high_resolution_clock;

    int                      descriptor;
    std::atomic<std::size_t> timeout_id;
    std::mutex               mutex;
};

using event_loop = kqueue_loop;

}

#endif
