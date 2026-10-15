#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>

#include "coro/async_scope.hpp"
#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/event_loop.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"

namespace net::io
{

using clock = std::chrono::steady_clock;

// scheduler runs tasks on a thread pool, and resumes them when the handles they wait on are ready.
//
// Lifecycle:
// - spawn() starts tasks, which the scheduler owns until they finish.
// - run() waits for and dispatches I/O on the calling thread, until the scheduler is stopped and its tasks have
//   finished.
// - request_stop() refuses new tasks, and cancels every pending (and future) wait (including wait_on() and
//   sleep_until()) with std::errc::operation_canceled, so tasks can finish by themselves. Code after the waits still runs (e.g. to clean
//   up), and reads/writes that don't need to wait still work.
// - shutdown() stops the scheduler, and blocks until its tasks have finished and run() has returned.
//
// A scheduler can't be restarted once stopped.
class scheduler
{
    struct waiter;

    using timer_list   = std::multimap<clock::time_point, waiter*>;
    using watched_list = std::multimap<const std::atomic<std::uint64_t>*, waiter*>;

    // waiter is a pending wait, owned by the suspended coroutine's wait_operation. It's one of:
    // - a wait for a handle (op is read or write).
    // - a wait for a notification about word (op is none, and word is set).
    // - a sleep, until the deadline (op is none, and word isn't set).
    struct waiter
    {
        handle                                fd;
        poll_op                               op;
        std::optional<clock::time_point>      deadline;
        std::coroutine_handle<>               awaiting{nullptr};
        std::error_condition                  result{};
        std::optional<timer_list::iterator>   timer{};
        const std::atomic<std::uint64_t>*     word     = nullptr;
        std::uint64_t                         expected = 0;
        std::optional<watched_list::iterator> watch{};
    };

    // registration holds the waiters for a handle: at most one per operation.
    struct registration
    {
        waiter* read  = nullptr;
        waiter* write = nullptr;
    };

public:
    // wait_operation waits for a handle to be ready for an operation, returning the error that stopped it, if any:
    // - std::errc::timed_out if the deadline passed first.
    // - std::errc::operation_canceled if the scheduler was stopped, or the handle deregistered.
    // - std::errc::bad_file_descriptor if the handle isn't registered.
    // - std::errc::device_or_resource_busy if another coroutine is already waiting for the same operation.
    class [[nodiscard]] wait_operation
    {
        friend class scheduler;

        wait_operation(scheduler* sched, handle fd, poll_op op, std::optional<clock::time_point> deadline) noexcept
            : sched{sched}
            , state{.fd = fd, .op = op, .deadline = deadline}
        {}

        wait_operation(scheduler*                        sched,
                       const std::atomic<std::uint64_t>* word,
                       std::uint64_t                     expected,
                       std::optional<clock::time_point>  deadline) noexcept
            : sched{sched}
            , state{.fd = -1, .op = poll_op::none, .deadline = deadline, .word = word, .expected = expected}
        {}

    public:
        wait_operation(const wait_operation&)            = delete;
        wait_operation& operator=(const wait_operation&) = delete;

        wait_operation(wait_operation&&)            = delete;
        wait_operation& operator=(wait_operation&&) = delete;

        ~wait_operation() = default;

        [[nodiscard]] bool await_ready() const noexcept { return false; }
        bool               await_suspend(std::coroutine_handle<> awaiting) noexcept
        {
            return sched->suspend(state, awaiting);
        }
        [[nodiscard]] std::error_condition await_resume() const noexcept { return state.result; }

    private:
        scheduler* sched;
        waiter     state;
    };

    // NOTE: the default logger isn't registered with spdlog, which only allows one logger per name.
    explicit scheduler(std::shared_ptr<coro::thread_pool>     workers,
              const std::shared_ptr<spdlog::logger>& logger
              = std::make_shared<spdlog::logger>("scheduler", std::make_shared<spdlog::sinks::null_sink_mt>()));

    scheduler(const scheduler&)            = delete;
    scheduler& operator=(const scheduler&) = delete;

    scheduler(scheduler&&)            = delete;
    scheduler& operator=(scheduler&&) = delete;

    // Calls shutdown(): must not be destroyed from one of its own tasks.
    ~scheduler() noexcept;

    // register_handle must be called before waiting on a handle, and deregister_handle before closing it.
    // deregister_handle cancels any waits on the handle.
    void register_handle(handle fd);
    void deregister_handle(handle fd) noexcept;

    // spawn starts task on the thread pool. If the scheduler is stopping, the task is destroyed without being run, and
    // false is returned.
    //
    // Exceptions that escape the task are logged.
    bool spawn(coro::task<> task);

    wait_operation wait(handle fd, poll_op op) noexcept { return {this, fd, op, std::nullopt}; }
    wait_operation wait_until(handle fd, poll_op op, clock::time_point deadline) noexcept
    {
        return {this, fd, op, deadline};
    }
    wait_operation wait_until(handle fd, poll_op op, std::optional<clock::time_point> deadline) noexcept
    {
        return {this, fd, op, deadline};
    }
    wait_operation wait_for(handle fd, poll_op op, clock::duration timeout) noexcept
    {
        return {this, fd, op, clock::now() + timeout};
    }

    // wait_on waits for notify_one() or notify_all() to be called with word, unless word no longer holds expected (so
    // nothing needs to wait for it). It's how a task waits for something another task does: e.g. to wait for a
    // condition, read word, check the condition, and wait unless it holds; and to notify, change the condition and
    // word (in that order), then notify. Then a notification between the check and the wait isn't lost, as it changed
    // word first.
    //
    // It fails with std::errc::timed_out if the deadline passes first, or std::errc::operation_canceled if the
    // scheduler is stopped. Waiters are notified in the order they started waiting.
    wait_operation wait_on(const std::atomic<std::uint64_t>& word,
                           std::uint64_t                     expected,
                           std::optional<clock::time_point>  deadline = std::nullopt) noexcept
    {
        return {this, &word, expected, deadline};
    }

    void notify_one(const std::atomic<std::uint64_t>& word) noexcept;
    void notify_all(const std::atomic<std::uint64_t>& word) noexcept;

    // sleep_until waits until deadline. It fails with std::errc::operation_canceled if the scheduler is stopped first.
    wait_operation sleep_until(clock::time_point deadline) noexcept { return {this, nullptr, 0, deadline}; }

    // run_blocking runs func on a thread set aside for blocking calls (e.g. getaddrinfo()), so it doesn't hold up the
    // worker threads, and then continues on the worker threads. Stopping the scheduler doesn't interrupt func.
    template<std::invocable F>
    coro::task<std::invoke_result_t<F>> run_blocking(F func)
    {
        using result_type = std::invoke_result_t<F>;

        co_await blocking->schedule();

        // the awaiting coroutine must continue on the workers, even if func throws.
        std::exception_ptr error;

        if constexpr (std::is_void_v<result_type>)
        {
            try
            {
                std::invoke(std::move(func));
            }
            catch (...)
            {
                error = std::current_exception();
            }

            co_await workers->schedule();
            if (error) std::rethrow_exception(error);
        }
        else
        {
            std::optional<result_type> result;

            try
            {
                result.emplace(std::invoke(std::move(func)));
            }
            catch (...)
            {
                error = std::current_exception();
            }

            co_await workers->schedule();
            if (error) std::rethrow_exception(error);
            co_return std::move(*result);
        }
    }

    void run();

    // run_to_completion spawns task, and runs the scheduler on the calling thread until it finishes. The scheduler is
    // then stopped, and once its other tasks have finished, the task's result is returned (or its exception thrown).
    template<typename T>
    T run_to_completion(coro::task<T> task)
    {
        using stored = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

        std::optional<stored> value;
        std::exception_ptr    error;

        // parameters rather than captures: the lambda object is destroyed before the coroutine finishes.
        auto wrapper = [](scheduler* self, coro::task<T> task, std::optional<stored>* value, std::exception_ptr* error)
            -> coro::task<>
        {
            try
            {
                if constexpr (std::is_void_v<T>)
                {
                    co_await task;
                    value->emplace();
                }
                else
                {
                    value->emplace(co_await std::move(task));
                }
            }
            catch (...)
            {
                *error = std::current_exception();
            }

            self->request_stop();
        };

        if (!spawn(wrapper(this, std::move(task), &value, &error)))
            throw std::logic_error{"run_to_completion: the scheduler is stopped"};

        run();

        if (error) std::rethrow_exception(error);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): the task either set it, or threw.
        if constexpr (!std::is_void_v<T>) return std::move(*value);
    }

    // request_stop stops the scheduler, without waiting for its tasks: it may be called from one of them.
    void request_stop() noexcept;

    [[nodiscard]] bool stop_requested() const noexcept;

    // shutdown stops the scheduler, and blocks until its tasks have finished, and run() has returned (if running). It
    // must not be called from one of the scheduler's own tasks, which would then wait for itself.
    void shutdown() noexcept;

private:
    using completions = std::vector<std::coroutine_handle<>>;

    bool suspend(waiter& state, std::coroutine_handle<> awaiting) noexcept;

    // Must be called while holding mu.
    void complete(waiter* state, std::error_condition result, completions& done) noexcept;
    void rearm(handle fd, const registration& reg, completions& done) noexcept;
    void dispatch(std::span<const ready_event> events, completions& done) noexcept;
    void notify(const std::atomic<std::uint64_t>& word, bool all) noexcept;
    void expire_timers(completions& done) noexcept;

    void resume(const completions& done) noexcept;

    std::shared_ptr<coro::thread_pool> workers;
    std::unique_ptr<coro::thread_pool> blocking;
    std::shared_ptr<spdlog::logger>    logger;
    detail::event_loop                 loop;

    mutable std::mutex                       mu;
    std::unordered_map<handle, registration> registrations;
    timer_list                               timers;
    watched_list                             watched;
    bool                                     stopping = false;

    std::atomic<bool> running = false;

    // NOTE: declared last, so it's destroyed first: tasks use the rest of the scheduler until they finish.
    coro::async_scope scope;
};

}
