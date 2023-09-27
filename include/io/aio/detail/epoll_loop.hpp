#pragma once

#include <array>
#include <chrono>
#include <concepts>

#include "config.hpp"

#ifdef NET_HAS_EPOLL

#    include <sys/epoll.h>
#    include <sys/eventfd.h>

#    include "io/aio/poll.hpp"

namespace net::io::aio::detail
{

using namespace std::chrono_literals;

class epoll_loop
{
public:
    epoll_loop();

    epoll_loop(const epoll_loop&)            = delete;
    epoll_loop& operator=(const epoll_loop&) = delete;

    epoll_loop(epoll_loop&&)            = delete;
    epoll_loop& operator=(epoll_loop&&) = delete;

    ~epoll_loop();

    void poll(int fd, poll_op op, std::chrono::milliseconds timeout = 0ms);

    template<typename OnScheduled, typename OnTimeout, typename OnEvent>
    void process_events(OnScheduled&&             on_scheduled,
                        OnTimeout&&               on_timeout,
                        OnEvent&&                 on_event,
                        std::chrono::milliseconds timeout = 0ms)
    {
        auto count =
            epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), static_cast<int>(timeout.count()));

        for (auto i = 0u; i < static_cast<std::size_t>(count); ++i)
        {
            epoll_event& event      = events[i];
            auto*        handle_ptr = event.data.ptr;

            if (handle_ptr == schedule_ptr)
            {
                on_scheduled();
            }
            else if (handle_ptr == timer_ptr)
            {
                // TODO: process timeouts
                on_timeout();
            }
            else if (handle_ptr == shutdown_ptr) [[unlikely]]
            {
                // noop
            }
            else
            {
                // TODO: process an actual event
                on_event();
            }
        }
    }

    void clear_schedule() noexcept;

    void shutdown() noexcept;

private:
    // unique identifiers for the different event handles
    static constexpr int schedule_value = 1;
    static constexpr int timer_value    = 1;
    static constexpr int shutdown_value = 1;

    static const constexpr void* const schedule_ptr = &schedule_value;
    static const constexpr void* const timer_ptr    = &timer_value;
    static const constexpr void* const shutdown_ptr = &shutdown_value;

    int epoll_fd;
    int schedule_fd;
    int timer_fd;
    int shutdown_fd;

    std::array<epoll_event, 16> events;
};

}

#endif
