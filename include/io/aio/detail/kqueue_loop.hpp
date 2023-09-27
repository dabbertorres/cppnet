#pragma once

#include "config.hpp"

#ifdef NET_HAS_KQUEUE

#    include <chrono>
#    include <coroutine>
#    include <functional>
#    include <mutex>
#    include <optional>
#    include <queue>
#    include <utility>
#    include <vector>

#    include <sys/event.h>
#    include <sys/types.h>

#    include "io/aio/poll.hpp"

namespace net::io::aio::detail
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

    void queue(wait_for&& trigger, std::chrono::milliseconds timeout = 0ms);

    void dispatch();

    /* template<typename OnScheduled, typename OnTimeout, typename OnEvent> */
    /* void process_events(OnScheduled&&             on_scheduled, */
    /*                     OnTimeout&&               on_timeout, */
    /*                     OnEvent&&                 on_event, */
    /*                     std::chrono::milliseconds timeout = 0ms) */
    /* { */
    /* } */

    /* void clear_schedule() noexcept; */

    void shutdown() noexcept;

private:
    using clock      = std::chrono::high_resolution_clock;
    using time_point = clock::time_point;

    struct timeout
    {
        std::coroutine_handle<>        handle;
        std::chrono::time_point<clock> expires_at;

        friend constexpr bool operator==(const timeout& lhs, const timeout& rhs) noexcept
        {
            return lhs.handle == rhs.handle;
        }
    };

    struct timeout_greater
    {
        bool operator()(const timeout& lhs, const timeout& rhs) noexcept { return lhs.expires_at > rhs.expires_at; }
    };

    // Use a single timer kevent that gets updated to the next timeout, rather than a timer kevent
    // for every single timeout.
    // This avoids any potential issues due to a system's max amount of timers.
    using timeout_queue = std::priority_queue<timeout, std::vector<timeout>, timeout_greater>;

    void                         queue_next_timeout() noexcept;
    std::optional<struct kevent> build_timeout_event(std::coroutine_handle<>   handle,
                                                     std::chrono::milliseconds timeout) noexcept;

    int           descriptor;
    uintptr_t     timeout_descriptor;
    timeout_queue timeouts;
    std::mutex    mutex;
};

}

#endif
