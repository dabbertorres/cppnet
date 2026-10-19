#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
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

namespace detail
{

    // blocking_result is what scheduler::run_blocking(func, stop) gets from func: given the token, if it takes one.
    template<typename F>
    struct blocking_result
    {
        using type = std::invoke_result_t<F>;
    };

    template<typename F>
        requires std::invocable<F, std::stop_token>
    struct blocking_result<F>
    {
        using type = std::invoke_result_t<F, std::stop_token>;
    };

    template<typename F>
    using blocking_result_t = blocking_result<F>::type;

}

// scheduler runs tasks on a thread pool, and resumes them when the handles they wait on are ready.
//
// Lifecycle:
// - spawn() starts tasks, which the scheduler owns until they finish.
// - The thread pool's idle workers wait for and dispatch I/O (see coro::thread_pool::poller), so a task readied by I/O
//   continues on the worker that saw it. If the pool already polls for another scheduler, run() does it instead.
// - run() blocks until the scheduler is stopped and its tasks have finished.
// - request_stop() refuses new tasks, and cancels every pending (and future) wait (including wait_on() and
//   sleep_until()) with std::errc::operation_canceled, so tasks can finish by themselves. Code after the waits still
//   runs (e.g. to clean up), and reads/writes that don't need to wait still work. Then it stops stop_token(), for what
//   isn't waiting on the scheduler.
// - A wait can also be canceled alone: the overloads that take a std::stop_token end with
//   std::errc::operation_canceled when stop is requested (see stoppable_wait_operation), as do sockets' reads and
//   writes given one (socket::set_stop_token()).
// - shutdown() stops the scheduler, and blocks until its tasks have finished and run() has returned.
//
// A scheduler can't be restarted once stopped.
class scheduler : private coro::thread_pool::poller
{
public:
    class handle_operation;
    class call_base;

private:
    struct waiter;

    using watched_list = std::multimap<const std::atomic<std::uint64_t>*, waiter*>;

    // timer_queue holds deadlines in order, each with what's waiting for it (T: a waiter, or a call_base). It keeps the
    // nodes of (a few of) the timers removed, for the next ones added, so adding one doesn't allocate each time: e.g.
    // for each read with a deadline.
    template<class T>
    class timer_queue
    {
    public:
        using timer_list = std::multimap<clock::time_point, T*>;
        using iterator   = timer_list::iterator;

        iterator add(clock::time_point deadline, T* state)
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

        timer_list                                  timers;
        std::vector<typename timer_list::node_type> spares;
    };

    using waiter_timers = timer_queue<waiter>;
    using call_timers   = timer_queue<call_base>;

    // waiter is a pending wait, owned by the suspended coroutine's wait_operation. It's one of:
    // - a wait for a handle (op is read or write).
    // - a wait for a notification about word (op is none, and word is set).
    // - a sleep, until the deadline (op is none, and word isn't set).
    struct waiter
    {
        handle  fd;
        poll_op op;

        // canceled is whether its stop token was stopped before it began waiting, so it mustn't (see stop_wait()).
        // Guarded by its shard's lock (for a handle's), or timers_mu.
        bool canceled = false;

        std::optional<clock::time_point>       deadline;
        std::coroutine_handle<>                awaiting{nullptr};
        std::error_code                        result{};
        std::optional<waiter_timers::iterator> timer{}; // guarded by its shard's lock, or (if not a handle's) timers_mu
        const std::atomic<std::uint64_t>*      word     = nullptr;
        std::uint64_t                          expected = 0;
        std::optional<watched_list::iterator>  watch{};

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
        waiter_timers                            timers;

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

    // canceler is the stop callback of a wait given a stop token: it cancels the wait (see stop_wait()).
    struct canceler
    {
        scheduler* sched;
        waiter*    state;

        void operator()() const noexcept { sched->stop_wait(*state); }
    };

    using stop_registration = std::stop_callback<canceler>;

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

        // begin waits for fd to be ready for op (read or write), until deadline, or until stop is requested, and then
        // performs this (as often as it takes) and resumes awaiting. It returns whether awaiting must suspend: false if
        // the wait failed straight away (see error()), or the handle was ready, and this has been performed already.
        // Once it's finished, end_wait() must be called before the next begin().
        bool begin(scheduler&                       sched,
                   handle                           fd,
                   poll_op                          op,
                   std::optional<clock::time_point> deadline,
                   std::coroutine_handle<>          awaiting,
                   const std::stop_token&           stop = {}) noexcept;

        // end_wait forgets the wait's stop token (waiting for its callback, if that's running on another thread): once
        // the wait has finished, before its result is taken.
        void end_wait() noexcept { on_stop.reset(); }

        // error is why the wait ended without this finishing, if it did: e.g. std::errc::timed_out, or
        // std::errc::operation_canceled if the scheduler was stopped, the handle deregistered, or stop requested. A
        // wait canceled by its stop token hasn't been performed since it was last tried (both happen under the
        // scheduler's lock for the handle), so e.g. a read canceled while it waits has read nothing.
        [[nodiscard]] std::error_code error() const noexcept { return state.result; }

        // set_deadline moves the wait's deadline: only from perform(), e.g. as it has made progress. (The limit, if
        // any, still applies: see limit_deadline().)
        void set_deadline(std::optional<clock::time_point> deadline) noexcept
        {
            own_deadline   = deadline;
            state.deadline = earliest(deadline, limit);
        }

        // limit_deadline limits the deadlines of this operation's waits for fd (the earlier of the two applies): those
        // that begin from now, and the wait in progress, if any, whose timer it moves. A wait reads the limit under the
        // scheduler's lock for the handle as it begins, which this holds too, so it may be called from another thread
        // than the waiting one, at any moment: e.g. to give a read that's already waiting a deadline (see
        // socket::set_read_deadline()). std::nullopt removes it.
        void limit_deadline(scheduler& sched, handle fd, std::optional<clock::time_point> limit) noexcept;

        // take_limit takes other's limit (e.g. as the object they belong to is moved): neither may be waiting.
        void take_limit(handle_operation& other) noexcept { limit = std::exchange(other.limit, std::nullopt); }

        // cancel_wait stops waiting (if it still is), without resuming the coroutine that began it, and calls
        // end_wait(): for when that's being destroyed. NOTE: it must not race with the wait finishing (which resumes
        // the coroutine), as the stop token's callback may make it.
        void cancel_wait() noexcept;

    private:
        friend class scheduler;

        waiter                           state{};
        scheduler*                       sched = nullptr;
        std::optional<stop_registration> on_stop;

        // own_deadline is the deadline begin() was given (or set_deadline() moved it to), and limit, limit_deadline()'s
        // (guarded by the scheduler's lock for the handle): a wait's deadline is the earlier of the two.
        std::optional<clock::time_point> own_deadline;
        std::optional<clock::time_point> limit;
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
        bool await_suspend(std::coroutine_handle<> awaiting) noexcept { return sched->suspend(state, awaiting); }
        [[nodiscard]] std::error_code await_resume() const noexcept { return state.result; }

    private:
        scheduler* sched;
        waiter     state;
    };

    // stoppable_wait_operation is a wait_operation given a stop token: requesting stop ends the wait with
    // std::errc::operation_canceled, unless it has already ended (then the wait's own result stands). Stop requested
    // before the wait begins fails it at once. A token that can't be stopped (e.g. a default-constructed one) is as
    // none. It registers a std::stop_callback while it waits (a few atomic operations, nothing allocated), whose
    // callback takes the scheduler's lock for the wait: so requesting stop must not happen holding one of the
    // scheduler's locks (e.g. in a handle_operation's perform()).
    class [[nodiscard]] stoppable_wait_operation
    {
        friend class scheduler;

        stoppable_wait_operation(scheduler*                       sched,
                                 handle                           fd,
                                 poll_op                          op,
                                 std::optional<clock::time_point> deadline,
                                 std::stop_token                  stop) noexcept
            : sched{sched}
            , state{.fd = fd, .op = op, .deadline = deadline}
            , stop{std::move(stop)}
        {}

        stoppable_wait_operation(scheduler*                        sched,
                                 const std::atomic<std::uint64_t>* word,
                                 std::uint64_t                     expected,
                                 std::optional<clock::time_point>  deadline,
                                 std::stop_token                   stop) noexcept
            : sched{sched}
            , state{.fd = -1, .op = poll_op::none, .deadline = deadline, .word = word, .expected = expected}
            , stop{std::move(stop)}
        {}

    public:
        stoppable_wait_operation(const stoppable_wait_operation&)            = delete;
        stoppable_wait_operation& operator=(const stoppable_wait_operation&) = delete;

        stoppable_wait_operation(stoppable_wait_operation&&)            = delete;
        stoppable_wait_operation& operator=(stoppable_wait_operation&&) = delete;

        ~stoppable_wait_operation() = default;

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiting) noexcept
        {
            // NOTE: registered before the wait takes the scheduler's lock, which the callback takes too: so a callback
            //       that runs first (e.g. here, as stop was already requested) finds it not waiting yet, and marks it
            //       canceled, which the wait then sees (see stop_wait()).
            if (stop.stop_possible()) on_stop.emplace(std::move(stop), canceler{.sched = sched, .state = &state});
            return sched->suspend(state, awaiting);
        }

        std::error_code await_resume() noexcept
        {
            // NOTE: waits for the callback, if it's running on another thread (having ended the wait, it only has to
            //       return), so it's done with state.
            on_stop.reset();
            return state.result;
        }

    private:
        scheduler*                       sched;
        waiter                           state;
        std::stop_token                  stop;
        std::optional<stop_registration> on_stop;
    };

    // call_base is a callback the scheduler calls at a deadline, rather than a coroutine it resumes: call_at()'s call,
    // without the callback's type. It's its caller's (nothing is allocated for it), so it can't be moved, and mustn't
    // outlive the scheduler.
    class call_base
    {
    public:
        call_base(const call_base&)            = delete;
        call_base& operator=(const call_base&) = delete;

        call_base(call_base&&)            = delete;
        call_base& operator=(call_base&&) = delete;

        // cancel cancels the call, returning whether it was still to be made (and now won't be). If it's being made on
        // another thread, cancel() waits for the callback to return: so once cancel() has returned, the callback isn't
        // running, and what it refers to may go. (Unless it's called by the callback itself, which it doesn't wait
        // for.) It takes the scheduler's lock for its timers, so it mustn't be called holding one of the scheduler's
        // locks.
        bool cancel() noexcept;

    protected:
        call_base(scheduler* sched, void (*invoke)(call_base&) noexcept) noexcept
            : sched{sched}
            , invoke{invoke}
        {}

        ~call_base() = default;

        // schedule schedules the call for deadline: once, by the derived class's constructor, once it's ready to be
        // called (as it may be at once, on another thread).
        void schedule(clock::time_point deadline) noexcept { sched->schedule_call(*this, deadline); }

    private:
        friend class scheduler;

        // stage is where the call is: not scheduled (the scheduler was stopping), or canceled; scheduled; being made;
        // or made.
        enum class stage : std::uint8_t
        {
            idle,
            pending,
            calling,
            called,
        };

        scheduler* sched;
        void (*invoke)(call_base&) noexcept;

        // guarded by the scheduler's timers_mu.
        std::optional<call_timers::iterator> timer{};
        stage                                status = stage::idle;
    };

    // call is call_at()'s call of func: destroying it cancels it (see call_base::cancel()).
    template<class F>
    class call final : public call_base
    {
    public:
        call(scheduler* sched, clock::time_point deadline, F func) noexcept(std::is_nothrow_move_constructible_v<F>)
            : call_base{sched, &call::invoke_func}
            , func{std::move(func)}
        {
            schedule(deadline);
        }

        call(const call&)            = delete;
        call& operator=(const call&) = delete;

        call(call&&)            = delete;
        call& operator=(call&&) = delete;

        ~call() { cancel(); }

    private:
        static void invoke_func(call_base& base) noexcept { std::invoke(static_cast<call&>(base).func); }

        F func;
    };

    // NOTE: the default logger isn't registered with spdlog, which only allows one logger per name.
    explicit scheduler(std::shared_ptr<coro::thread_pool>     workers,
                       const std::shared_ptr<spdlog::logger>& logger = std::make_shared<spdlog::logger>(
                           "scheduler", std::make_shared<spdlog::sinks::null_sink_mt>()));

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

    // pool is the thread pool the scheduler runs tasks on.
    [[nodiscard]] coro::thread_pool* pool() const noexcept { return workers.get(); }

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

    // These waits also end when stop is requested (see stoppable_wait_operation).
    stoppable_wait_operation wait(handle fd, poll_op op, std::stop_token stop) noexcept
    {
        return {this, fd, op, std::nullopt, std::move(stop)};
    }
    stoppable_wait_operation
    wait_until(handle fd, poll_op op, std::optional<clock::time_point> deadline, std::stop_token stop) noexcept
    {
        return {this, fd, op, deadline, std::move(stop)};
    }
    stoppable_wait_operation wait_for(handle fd, poll_op op, clock::duration timeout, std::stop_token stop) noexcept
    {
        return {this, fd, op, clock::now() + timeout, std::move(stop)};
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

    // wait_on(word, expected, deadline, stop) also ends when stop is requested (see stoppable_wait_operation).
    stoppable_wait_operation wait_on(const std::atomic<std::uint64_t>& word,
                                     std::uint64_t                     expected,
                                     std::optional<clock::time_point>  deadline,
                                     std::stop_token                   stop) noexcept
    {
        return {this, &word, expected, deadline, std::move(stop)};
    }

    void notify_one(const std::atomic<std::uint64_t>& word) noexcept;
    void notify_all(const std::atomic<std::uint64_t>& word) noexcept;

    // sleep_until waits until deadline. It fails with std::errc::operation_canceled if the scheduler is stopped first.
    wait_operation sleep_until(clock::time_point deadline) noexcept { return {this, nullptr, 0, deadline}; }

    // sleep_until(deadline, stop) also ends when stop is requested (see stoppable_wait_operation): e.g. a handler's
    // sleep, given its request's token.
    stoppable_wait_operation sleep_until(clock::time_point deadline, std::stop_token stop) noexcept
    {
        return {this, nullptr, 0, deadline, std::move(stop)};
    }

    // call_at calls func at deadline (or as soon as it can, if that's passed), unless the call it returns is canceled
    // or destroyed first: a timer that runs a callback rather than resuming a coroutine, for what needs no task of its
    // own (e.g. http::timeout's, which only requests stop). Nothing is allocated for it (the call holds func, and the
    // scheduler's timers keep the nodes of a few removed, for the next ones), and it's canceled by a lock and a
    // removal.
    //
    // func is called on whichever thread polls for the scheduler (one of its pool's workers, or the thread calling
    // run(): see run()), holding none of the scheduler's locks: so it may request stop on a source whose tokens the
    // scheduler's waits were given. It must be quick, and not block, as it holds up the thread's polling; and it
    // mustn't destroy its own call. If the scheduler is stopped first (or was already), func isn't called.
    template<class F>
        requires std::is_nothrow_invocable_v<std::decay_t<F>&>
    [[nodiscard]] call<std::decay_t<F>> call_at(clock::time_point deadline, F&& func)
    {
        return {this, deadline, std::forward<F>(func)};
    }

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

    // run_blocking(func, stop) is run_blocking(func), unless stop is requested before func starts: then func isn't
    // run, and it fails with std::errc::operation_canceled. Nothing interrupts func once it has started (that would
    // leave it running, with what it refers to, after the call returned), but a func that takes a std::stop_token is
    // given stop, to check as it goes. Once func has run, its result is returned, even if stop was requested meanwhile.
    template<typename F>
        requires std::invocable<F, std::stop_token> || std::invocable<F>
    coro::task<std::expected<detail::blocking_result_t<F>, std::error_code>> run_blocking(F func, std::stop_token stop)
    {
        using result_type = detail::blocking_result_t<F>;
        using expected    = std::expected<result_type, std::error_code>;

        // NOTE: not a coroutine itself, so this adds no frame to run_blocking(func)'s.
        return run_blocking(
            [func = std::move(func), stop = std::move(stop)] mutable -> expected
            {
                if (stop.stop_requested()) return std::unexpected(std::make_error_code(std::errc::operation_canceled));

                if constexpr (std::is_void_v<result_type>)
                {
                    if constexpr (std::invocable<F, std::stop_token>) std::invoke(std::move(func), std::move(stop));
                    else std::invoke(std::move(func));
                    return {};
                }
                else if constexpr (std::invocable<F, std::stop_token>)
                {
                    return std::invoke(std::move(func), std::move(stop));
                }
                else
                {
                    return std::invoke(std::move(func));
                }
            });
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

    // stop_token is stopped by request_stop(), once it has canceled the waits: for what should end with the scheduler
    // but doesn't wait on it (e.g. work between waits). Its callbacks run on the thread calling request_stop().
    [[nodiscard]] std::stop_token stop_token() const noexcept { return stopper.get_token(); }

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

    // earliest is the earlier of two deadlines, either of which may be none.
    [[nodiscard]] static std::optional<clock::time_point> earliest(std::optional<clock::time_point> a,
                                                                   std::optional<clock::time_point> b) noexcept
    {
        if (!a) return b;
        if (!b) return a;
        return *b < *a ? b : a;
    }

    // ready_for completes the waiter that slot (in sh) holds, now its handle's ready, or performs its operation, which
    // may keep it waiting (see handle_operation). Must be called holding sh's lock.
    void ready_for(shard& sh, direction& slot, completions& done) noexcept;

    // cancel stops a handle's waiter waiting (if it still is), without completing it.
    void cancel(waiter& state) noexcept;

    // stop_wait is a waiter's stop callback: it completes the waiter with std::errc::operation_canceled (and resumes
    // it on the pool) if it's waiting, or marks it canceled, for suspend() to fail it, if it hasn't begun. It takes the
    // waiter's lock, but is called by whoever requested stop (or the callback's constructor, if stop was requested
    // already), so never holding it.
    void stop_wait(waiter& state) noexcept;

    // retime replaces the timer of a waiter (whose handle is in sh) for its deadline. Must be called holding sh's lock.
    void retime(shard& sh, waiter& state) noexcept;

    // complete_handle_wait completes a handle's waiter, which slot (in sh) holds, returning the coroutine to resume.
    // Must be called holding sh's lock.
    [[nodiscard]] static std::coroutine_handle<>
    complete_handle_wait(shard& sh, direction& slot, std::error_code result) noexcept;

    // complete_other completes a waiter that isn't for a handle, returning the coroutine to resume. Must be called
    // holding timers_mu.
    [[nodiscard]] std::coroutine_handle<> complete_other(waiter* state, std::error_code result) noexcept;

    // schedule_call adds call to the calls, for deadline, unless the scheduler is stopping.
    void schedule_call(call_base& call, clock::time_point deadline) noexcept;

    // make_calls makes the calls whose deadlines have passed.
    void make_calls() noexcept;

    void dispatch(std::span<const ready_event> events, completions& done) noexcept;
    void notify(const std::atomic<std::uint64_t>& word, bool all) noexcept;
    // expire_timers completes the waits whose deadlines have passed, returning whether a call's has too (see
    // make_calls()).
    bool expire_timers(completions& done) noexcept;

    // next_deadline is the earliest deadline of any wait (as clock::rep): the most run() may wait for.
    [[nodiscard]] clock::rep next_deadline() const noexcept;

    // note_deadline interrupts run() if it's waiting for a later deadline than this new one.
    void note_deadline(clock::time_point deadline) noexcept;

    void resume(const completions& done) noexcept;
    void resume(std::coroutine_handle<> awaiting) noexcept;

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

    // timers_mu guards the timers of waits that aren't for handles (sleeps, and waits for notifications), the waits for
    // notifications (watched), and the calls (see call_at()).
    mutable std::mutex timers_mu;
    waiter_timers      timers;
    watched_list       watched;

    // calls are the calls pending (see call_at()), and calls_done is notified (holding timers_mu) when one has been
    // made, for call_base::cancel() to wait for one being made. Guarded by timers_mu.
    call_timers             calls;
    std::condition_variable calls_done;

    // sleeping_until is the deadline run() is waiting for (as clock::rep), so that a wait with an earlier one
    // interrupts it (see run()).
    std::atomic<clock::rep> sleeping_until = std::numeric_limits<clock::rep>::max();

    std::atomic<bool> stopping = false;

    // stopper is stop_token()'s source.
    std::stop_source stopper;

    std::atomic<bool> running = false;

    // workers_poll is whether the workers poll for I/O, rather than run().
    bool workers_poll = false;

    // NOTE: declared last, so it's destroyed first: tasks use the rest of the scheduler until they finish.
    coro::async_scope scope;
};

}
