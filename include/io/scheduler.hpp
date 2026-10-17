#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
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
// - The thread pool's idle workers wait for and dispatch I/O (see coro::thread_pool::poller), so a task readied by I/O
//   continues on the worker that saw it. If the pool already polls for another scheduler, run() does it instead.
// - run() blocks until the scheduler is stopped and its tasks have finished.
// - request_stop() refuses new tasks, and cancels every pending (and future) wait (including wait_on() and
//   sleep_until()) with std::errc::operation_canceled, so tasks can finish by themselves. Code after the waits still
//   runs (e.g. to clean up), and reads/writes that don't need to wait still work.
// - shutdown() stops the scheduler, and blocks until its tasks have finished and run() has returned.
//
// A scheduler can't be restarted once stopped.
class scheduler : private coro::thread_pool::poller
{
public:
    class handle_operation;

private:
    struct waiter;

    using timer_list   = std::multimap<clock::time_point, waiter*>;
    using watched_list = std::multimap<const std::atomic<std::uint64_t>*, waiter*>;

    // timer_queue holds waiters' deadlines in order. It keeps the nodes of (a few of) the timers removed, for the next
    // ones added, so adding one doesn't allocate each time: e.g. for each read with a deadline.
    class timer_queue
    {
    public:
        using iterator = timer_list::iterator;

        iterator add(clock::time_point deadline, waiter* state)
        {
            if (spares.empty())
            {
                // NOTE: reserved once, so that remove() doesn't allocate.
                if (spares.capacity() == 0) spares.reserve(max_spares);
                return timers.emplace(deadline, state);
            }

            auto node = std::move(spares.back());
            spares.pop_back();
            node.key()    = deadline;
            node.mapped() = state;
            return timers.insert(std::move(node));
        }

        void remove(iterator it) noexcept
        {
            auto node = timers.extract(it);
            if (spares.size() < spares.capacity()) spares.push_back(std::move(node));
        }

        [[nodiscard]] bool     empty() const noexcept { return timers.empty(); }
        [[nodiscard]] iterator begin() noexcept { return timers.begin(); }

        // earliest is the earliest deadline: there must be one.
        [[nodiscard]] clock::time_point earliest() const noexcept { return timers.begin()->first; }

    private:
        static constexpr std::size_t max_spares = 16;

        timer_list                         timers;
        std::vector<timer_list::node_type> spares;
    };

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
        std::error_code                       result{};
        std::optional<timer_list::iterator>   timer{}; // guarded by its shard's lock, or (if not a handle's) timers_mu
        const std::atomic<std::uint64_t>*     word     = nullptr;
        std::uint64_t                         expected = 0;
        std::optional<watched_list::iterator> watch{};

        // operation, for a wait for a handle, is what to perform once the handle's ready (see handle_operation).
        handle_operation* operation = nullptr;
    };

    // direction is a handle's state for one operation: the waiter, if any, and whether the handle became ready (since
    // the last wait) when nothing was waiting.
    struct direction
    {
        waiter* waiting = nullptr;
        bool    ready   = false;
    };

    struct registration
    {
        direction read;
        direction write;
    };

    // shard holds some of the handles' registrations (by fd), and their waiters' timers, so that waits on different
    // handles rarely share a lock.
    struct alignas(64) shard
    {
        std::mutex                               mu;
        std::unordered_map<handle, registration> registrations;
        timer_queue                              timers;

        // earliest is the earliest of the timers' deadlines (as clock::rep), for run() to read without the lock.
        std::atomic<clock::rep> earliest = std::numeric_limits<clock::rep>::max();

        // Must be called holding mu, once timers changes.
        void update_earliest() noexcept
        {
            earliest.store(timers.empty() ? std::numeric_limits<clock::rep>::max()
                                          : timers.earliest().time_since_epoch().count());
        }
    };

    static constexpr std::size_t shard_count = 64;

public:
    // handle_operation is an operation on a handle (e.g. a socket's recv()) that the scheduler performs once the
    // handle's ready, rather than resuming the waiting coroutine to try it again: so that coroutine is only resumed
    // once it's finished, and needn't be one of its own that tries and waits in turn (see socket's reads and writes).
    // It, register_handle() and deregister_handle() are for I/O objects (e.g. sockets), not applications.
    class handle_operation
    {
    public:
        handle_operation(const handle_operation&)                = delete;
        handle_operation(handle_operation&&) noexcept            = delete;
        handle_operation& operator=(const handle_operation&)     = delete;
        handle_operation& operator=(handle_operation&&) noexcept = delete;

        // perform tries the operation, returning whether it's finished (successfully or not): false if it would have
        // to wait (e.g. recv() failed with EAGAIN), when the scheduler waits for the handle to be ready again. It's
        // called with the scheduler's lock for the handle held, so it mustn't block, or use the scheduler.
        virtual bool perform() noexcept = 0;

    protected:
        handle_operation() noexcept = default;
        ~handle_operation()         = default;

        // begin waits for fd to be ready for op (read or write), until deadline, and then performs this (as often as it
        // takes) and resumes awaiting. It returns whether awaiting must suspend: false if the wait failed straight away
        // (see error()), or the handle was ready, and this has been performed already.
        bool begin(scheduler&                       sched,
                   handle                           fd,
                   poll_op                          op,
                   std::optional<clock::time_point> deadline,
                   std::coroutine_handle<>          awaiting) noexcept;

        // error is why the wait ended without this finishing, if it did: e.g. std::errc::timed_out, or
        // std::errc::operation_canceled if the scheduler was stopped, or the handle deregistered.
        [[nodiscard]] std::error_code error() const noexcept { return state.result; }

        // set_deadline moves the wait's deadline: only from perform(), e.g. as it has made progress.
        void set_deadline(std::optional<clock::time_point> deadline) noexcept { state.deadline = deadline; }

        // cancel_wait stops waiting (if it still is), without resuming the coroutine that began it: for when that's
        // being destroyed. NOTE: it must not race with the wait finishing (which resumes the coroutine).
        void cancel_wait() noexcept;

    private:
        friend class scheduler;

        waiter     state{};
        scheduler* sched = nullptr;
    };

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
        [[nodiscard]] std::error_code await_resume() const noexcept { return state.result; }

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

    // Calls shutdown(): must not be destroyed from one of its own tasks (or one of the pool's workers), which
    // terminates the program (as shutdown() throws).
    ~scheduler() noexcept override;

    // register_handle must be called before waiting on a handle, and deregister_handle before closing it.
    // deregister_handle cancels any waits on the handle. (For I/O objects, e.g. sockets, which call them.)
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
    // throws std::logic_error if it's called from one of the pool's workers (e.g. by one of the scheduler's tasks),
    // which would wait for itself: call request_stop() there instead.
    void shutdown();

private:
    using completions = std::vector<std::coroutine_handle<>>;

    bool suspend(waiter& state, std::coroutine_handle<> awaiting) noexcept;
    bool suspend_handle(waiter& state, std::coroutine_handle<> awaiting) noexcept;
    bool suspend_other(waiter& state, std::coroutine_handle<> awaiting) noexcept;

    shard& shard_for(handle fd) noexcept { return shards[static_cast<std::size_t>(fd) % shard_count]; }

    // ready_for completes the waiter that slot (in sh) holds, now its handle's ready, or performs its operation, which
    // may keep it waiting (see handle_operation). Must be called holding sh's lock.
    void ready_for(shard& sh, direction& slot, completions& done) noexcept;

    // cancel stops a handle's waiter waiting (if it still is), without completing it.
    void cancel(waiter& state) noexcept;

    // retime replaces the timer of a waiter (whose handle is in sh) for its deadline. Must be called holding sh's lock.
    void retime(shard& sh, waiter& state) noexcept;

    // complete_handle_wait completes a handle's waiter, which slot (in sh) holds. Must be called holding sh's lock.
    void complete_handle_wait(shard& sh, direction& slot, std::error_code result, completions& done) noexcept;

    // complete_other completes a waiter that isn't for a handle. Must be called holding timers_mu.
    void complete_other(waiter* state, std::error_code result, completions& done) noexcept;

    void dispatch(std::span<const ready_event> events, completions& done) noexcept;
    void notify(const std::atomic<std::uint64_t>& word, bool all) noexcept;
    void expire_timers(completions& done) noexcept;

    // next_deadline is the earliest deadline of any wait (as clock::rep): the most run() may wait for.
    [[nodiscard]] clock::rep next_deadline() const noexcept;

    // note_deadline interrupts run() if it's waiting for a later deadline than this new one.
    void note_deadline(clock::time_point deadline) noexcept;

    void resume(const completions& done) noexcept;

    // poll waits for I/O (if block, until some is ready, the next deadline, or wake() is called), and dispatches it,
    // and the timers that expired, to done.
    void poll(bool block, completions& done) noexcept override;
    void wake() noexcept override;

    std::shared_ptr<coro::thread_pool> workers;
    std::unique_ptr<coro::thread_pool> blocking;
    std::shared_ptr<spdlog::logger>    logger;
    detail::event_loop                 loop;

    // Lock order: a shard's mu, then timers_mu.
    std::array<shard, shard_count> shards;

    // timers_mu guards the timers of waits that aren't for handles (sleeps, and waits for notifications), and the
    // waits for notifications (watched).
    mutable std::mutex timers_mu;
    timer_queue        timers;
    watched_list       watched;

    // sleeping_until is the deadline run() is waiting for (as clock::rep), so that a wait with an earlier one
    // interrupts it (see run()).
    std::atomic<clock::rep> sleeping_until = std::numeric_limits<clock::rep>::max();

    std::atomic<bool> stopping = false;

    std::atomic<bool> running = false;

    // workers_poll is whether the workers poll for I/O, rather than run().
    bool workers_poll = false;

    // NOTE: declared last, so it's destroyed first: tasks use the rest of the scheduler until they finish.
    coro::async_scope scope;
};

}
