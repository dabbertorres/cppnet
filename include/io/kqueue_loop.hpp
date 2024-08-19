#pragma once

#include "config.hpp" // IWYU pragma: keep

#ifdef NET_HAS_KQUEUE

#    include <atomic>
#    include <chrono>
#    include <coroutine>
#    include <cstdint>
#    include <ctime>
#    include <memory>

#    include <sys/event.h>
#    include <sys/types.h>

#    include <spdlog/common.h>
#    include <spdlog/logger.h>
#    include <spdlog/sinks/null_sink.h>
#    include <spdlog/spdlog.h>

#    include "coro/generator.hpp"
#    include "coro/task.hpp"
#    include "io/event.hpp"
#    include "io/io.hpp"
#    include "io/poll.hpp"

namespace net::io::detail
{

using namespace std::chrono_literals;

class kqueue_loop
{
public:
    kqueue_loop(std::shared_ptr<spdlog::logger> logger = spdlog::null_logger_mt("kqueue_loop"));

    kqueue_loop(const kqueue_loop&)            = delete;
    kqueue_loop& operator=(const kqueue_loop&) = delete;

    kqueue_loop(kqueue_loop&&) noexcept            = delete;
    kqueue_loop& operator=(kqueue_loop&&) noexcept = delete;

    ~kqueue_loop();

    void register_handle(io_handle handle) { /* noop - for now? */ }
    void deregister_handle(io_handle handle) { /* noop - for now? */ }

    coro::task<result> queue(io_handle handle, poll_op op, std::chrono::milliseconds timeout);

    coro::generator<event> dispatch() const;

    void shutdown() noexcept;

private:
    using clock = std::chrono::high_resolution_clock;

    class operation
    {
        friend class kqueue_loop;

        explicit operation(kqueue_loop* loop, io_handle handle, poll_op op, std::chrono::milliseconds timeout) noexcept
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
        kqueue_loop*                   loop;
        io_handle                      handle;
        poll_op                        op;
        std::chrono::milliseconds      timeout;
        std::coroutine_handle<promise> awaiting{nullptr};
    };

    friend class operation;

    void
    queue(std::coroutine_handle<promise> awaiting, io_handle handle, poll_op op, std::chrono::milliseconds timeout);

    struct kevent
    make_io_kevent(std::coroutine_handle<promise> awaiting, io_handle handle, std::int16_t filter) const noexcept;
    struct kevent make_timeout_kevent(std::coroutine_handle<promise> awaiting,
                                      std::chrono::milliseconds      timeout) noexcept;

    std::atomic<bool>               running;
    int                             descriptor;
    std::atomic<std::size_t>        timeout_id;
    std::shared_ptr<spdlog::logger> logger;
};

}

#endif
