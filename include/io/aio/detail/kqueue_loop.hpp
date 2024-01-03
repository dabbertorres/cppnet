#pragma once

#include "config.hpp"

#ifdef NET_HAS_KQUEUE

#    include <atomic>
#    include <chrono>
#    include <coroutine>
#    include <ctime>
#    include <functional>
#    include <mutex>
#    include <system_error>

#    include <spdlog/spdlog.h>
#    include <sys/event.h>
#    include <sys/types.h>

#    include "io/io.hpp"

#    include "io/aio/detail/event_handler.hpp"
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

    void queue(const wait_for& trigger);

    template<EventHandler OnEvent>
    void dispatch(OnEvent&& on_event)
    {
        std::array<struct kevent, 16> events{};

        auto num_events = kevent(descriptor, nullptr, 0, events.data(), events.size(), nullptr);
        if (num_events == -1)
        {
            // TODO: throw/return an error?
            spdlog::error("error queueing reading new events: {}", errno);
            return;
        }

        for (auto i = 0u; i < static_cast<std::size_t>(num_events); ++i)
        {
            auto& ev = events[i];
            switch (ev.filter)
            {
            case EVFILT_TIMER:
            {
                auto handle = std::coroutine_handle<promise>::from_address(events[i].udata);

                result res = {
                    .count = 0,
                    .err   = make_error_condition(status_condition::timed_out),
                };

                std::invoke(std::forward<OnEvent>(on_event), handle, res);
                break;
            }

            case EVFILT_READ: [[fallthrough]];
            case EVFILT_WRITE:
            {
                auto handle = std::coroutine_handle<promise>::from_address(events[i].udata);

                result res = {
                    .count = static_cast<std::size_t>(ev.data),
                    .err   = {},
                };

                if ((ev.flags & EV_EOF) != 0)
                {
                    // Did it shutdown due to an error? Or did the socket just close?
                    res.err = ev.fflags != 0 ? std::make_error_condition(static_cast<std::errc>(ev.fflags))
                                             : make_error_condition(status_condition::closed);
                }

                std::invoke(std::forward<OnEvent>(on_event), handle, res);
                break;
            }

            default:
                // no-op
            }
        }
    }

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

