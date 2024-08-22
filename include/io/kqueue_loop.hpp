#pragma once

#include <tuple>

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
    kqueue_loop(const std::shared_ptr<spdlog::logger>& logger = spdlog::null_logger_mt("kqueue_loop"));

    kqueue_loop(const kqueue_loop&)            = delete;
    kqueue_loop& operator=(const kqueue_loop&) = delete;

    kqueue_loop(kqueue_loop&&) noexcept            = delete;
    kqueue_loop& operator=(kqueue_loop&&) noexcept = delete;

    ~kqueue_loop() noexcept;

    void register_handle(handle fd) { /* noop - for now? */ }
    void deregister_handle(handle fd) { /* noop - for now? */ }

    coro::task<result> queue(handle fd, poll_op op, std::chrono::milliseconds timeout);

    coro::generator<event> dispatch();

    void shutdown() noexcept;

private:
    using clock = std::chrono::high_resolution_clock;

    // It is left as an excercise to the reader to determine if this value has any special meaning.
    // (Hint: it doesn't really)
    static constexpr uintptr_t shutdown_ident = 0x6578'6974;

    class operation
    {
        friend class kqueue_loop;

        explicit operation(kqueue_loop* loop, handle fd, poll_op op, std::chrono::milliseconds timeout) noexcept
            : loop{loop}
            , fd{fd}
            , op{op}
            , timeout{timeout}
        {}

    public:
        constexpr bool await_ready() noexcept { return false; }
        result         await_resume() noexcept;
        void           await_suspend(std::coroutine_handle<promise> await_on) noexcept;

    private:
        kqueue_loop*                   loop;
        handle                         fd;
        poll_op                        op;
        std::chrono::milliseconds      timeout;
        std::coroutine_handle<promise> awaiting{nullptr};
    };

    friend class operation;

    std::tuple<event, bool> translate_kevent(const struct kevent& ev) const noexcept;

    void queue(std::coroutine_handle<promise> awaiting, handle fd, poll_op op, std::chrono::milliseconds timeout);

    struct kevent
    make_io_kevent(std::coroutine_handle<promise> awaiting, handle fd, std::int16_t filter) const noexcept;
    struct kevent make_timeout_kevent(std::coroutine_handle<promise> awaiting,
                                      std::chrono::milliseconds      timeout) noexcept;

    std::atomic<bool>               running;
    int                             descriptor;
    std::atomic<std::size_t>        timeout_id;
    std::shared_ptr<spdlog::logger> logger;
};

}

#endif
