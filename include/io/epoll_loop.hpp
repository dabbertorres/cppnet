#pragma once

#include "config.hpp"

#ifdef NET_HAS_EPOLL

#    include <atomic>
#    include <chrono>
#    include <coroutine>
#    include <memory>

#    include <sys/epoll.h>
#    include <sys/eventfd.h>

#    include <spdlog/logger.h>
#    include <spdlog/sinks/null_sink.h>

#    include "coro/generator.hpp"
#    include "coro/task.hpp"
#    include "io/event.hpp"
#    include "io/io.hpp"
#    include "io/poll.hpp"

namespace net::io::detail
{

using namespace std::chrono_literals;

class epoll_loop
{
public:
    epoll_loop(std::shared_ptr<spdlog::logger> logger = spdlog::null_logger_mt("kqueue_loop"));

    epoll_loop(const epoll_loop&)            = delete;
    epoll_loop& operator=(const epoll_loop&) = delete;

    epoll_loop(epoll_loop&&)            = delete;
    epoll_loop& operator=(epoll_loop&&) = delete;

    ~epoll_loop();

    coro::task<result>     queue(io_handle handle, poll_op op, std::chrono::milliseconds timeout);
    coro::generator<event> dispatch() const;

    void shutdown() noexcept;

private:
    class operation
    {
        friend class epoll_loop;

        explicit operation(epoll_loop* loop, io_handle handle, poll_op op, std::chrono::milliseconds timeout) noexcept
            : loop{loop}
            , handle{handle}
            , op{op}
            , timeout{timeout}
        {}

    public:
        constexpr bool await_ready() noexcept { return false; }
        result         await_resume() noexcept;
        void           await_suspend(std::coroutine_handle<promise> handle) noexcept;

    private:
        epoll_loop*                    loop;
        io_handle                      handle;
        poll_op                        op;
        std::chrono::milliseconds      timeout;
        std::coroutine_handle<promise> awaiting{nullptr};
    };

    friend class operation;

    void
    queue(std::coroutine_handle<promise> awaiting, io_handle handle, poll_op op, std::chrono::milliseconds timeout);

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

    std::atomic<bool>               running;
    std::shared_ptr<spdlog::logger> logger;
};

}

#endif
