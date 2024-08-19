#pragma once

#include "config.hpp"

#ifdef NET_HAS_EPOLL

#    include <atomic>
#    include <chrono>
#    include <coroutine>
#    include <memory>
#    include <mutex>
#    include <queue>

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
    epoll_loop(const std::shared_ptr<spdlog::logger>& logger = spdlog::null_logger_mt("epoll_loop"));

    epoll_loop(const epoll_loop&)            = delete;
    epoll_loop& operator=(const epoll_loop&) = delete;

    epoll_loop(epoll_loop&&)            = delete;
    epoll_loop& operator=(epoll_loop&&) = delete;

    ~epoll_loop();

    // TODO: expose these on the scheduler
    void register_handle(io_handle handle);
    void deregister_handle(io_handle handle);

    coro::task<result>                   queue(io_handle handle, poll_op op, std::chrono::milliseconds timeout);
    [[nodiscard]] coro::generator<event> dispatch();

    void shutdown() noexcept;

private:
    using clock = std::chrono::high_resolution_clock;

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
        [[nodiscard]] bool await_ready() const noexcept;
        result             await_resume() noexcept;
        void               await_suspend(std::coroutine_handle<promise> await_on);

    private:
        epoll_loop*                    loop;
        io_handle                      handle;
        poll_op                        op;
        std::chrono::milliseconds      timeout;
        std::coroutine_handle<promise> awaiting{nullptr};
    };

    friend class operation;

    struct timeout_operation
    {
        std::coroutine_handle<promise> handle;
        clock::time_point              timeout_at;

        constexpr bool operator<(const timeout_operation& other) const noexcept
        {
            return timeout_at < other.timeout_at;
        }
    };

    void
    queue(std::coroutine_handle<promise> awaiting, io_handle handle, poll_op op, std::chrono::milliseconds timeout);
    void update_timer(std::chrono::milliseconds timeout);

    int epoll_fd;
    int timer_fd;
    int shutdown_fd;

    std::atomic<bool>                      running;
    std::shared_ptr<spdlog::logger>        logger;
    std::priority_queue<timeout_operation> timeout_list;
    std::mutex                             timeout_list_mu;
};

}

#endif
