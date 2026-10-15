#include "io/scheduler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <spdlog/logger.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/event_loop.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"

namespace
{

net::coro::task<> start_on(net::coro::thread_pool* workers, net::coro::task<> task)
{
    co_await workers->schedule();
    co_await task;
}

net::io::poll_op interest(const auto& reg) noexcept
{
    auto op = net::io::poll_op::none;
    if (reg.read != nullptr) op = op | net::io::poll_op::read;
    if (reg.write != nullptr) op = op | net::io::poll_op::write;
    return op;
}

}

namespace net::io
{

scheduler::scheduler(std::shared_ptr<coro::thread_pool> workers, const std::shared_ptr<spdlog::logger>& logger)
    : workers{std::move(workers)} // a few threads, so that one slow lookup doesn't hold up the rest.
    , blocking{std::make_unique<coro::thread_pool>(4)}
    , logger{logger}
    , scope{[this](const std::exception_ptr& ex)
            {
                try
                {
                    std::rethrow_exception(ex);
                }
                catch (const std::exception& err)
                {
                    this->logger->error("unhandled exception in task: {}", err.what());
                }
                catch (...)
                {
                    this->logger->error("unhandled exception in task");
                }
            },
            [this] { loop.interrupt(); }}
{}

scheduler::~scheduler() noexcept { shutdown(); }

void scheduler::register_handle(handle fd)
{
    std::scoped_lock lock{mu};

    if (registrations.contains(fd)) return;

    loop.add(fd);
    registrations.emplace(fd, registration{});
}

void scheduler::deregister_handle(handle fd) noexcept
{
    completions done;

    {
        std::scoped_lock lock{mu};

        auto it = registrations.find(fd);
        if (it == registrations.end()) return;

        auto canceled = std::make_error_condition(std::errc::operation_canceled);
        if (it->second.read != nullptr) complete(it->second.read, canceled, done);
        if (it->second.write != nullptr) complete(it->second.write, canceled, done);

        registrations.erase(it);
        loop.remove(fd);
    }

    resume(done);
}

bool scheduler::spawn(coro::task<> task) { return scope.spawn(start_on(workers.get(), std::move(task))); }

bool scheduler::suspend(waiter& state, std::coroutine_handle<> awaiting) noexcept
{
    std::unique_lock lock{mu};

    const auto fail = [&](std::errc err)
    {
        state.result = std::make_error_condition(err);
        return false; // don't suspend
    };

    if (stopping) return fail(std::errc::operation_canceled);

    // a wait for a notification, or a sleep.
    if (state.op == poll_op::none)
    {
        if (state.word != nullptr && state.word->load(std::memory_order::acquire) != state.expected)
        {
            // the notification already came.
            state.result = {};
            return false;
        }

        if (state.word == nullptr && !state.deadline) return fail(std::errc::invalid_argument);

        if (state.deadline && *state.deadline <= clock::now())
        {
            // a sleep that's over isn't an error.
            if (state.word == nullptr) return false;
            return fail(std::errc::timed_out);
        }

        if (state.word != nullptr) state.watch = watched.emplace(state.word, &state);
        state.awaiting = awaiting;

        bool earliest = false;
        if (state.deadline)
        {
            state.timer = timers.emplace(*state.deadline, &state);
            earliest    = *state.timer == timers.begin();
        }

        lock.unlock();

        if (earliest) loop.interrupt();
        return true;
    }

    if (state.op != poll_op::read && state.op != poll_op::write) return fail(std::errc::invalid_argument);

    auto it = registrations.find(state.fd);
    if (it == registrations.end()) return fail(std::errc::bad_file_descriptor);

    if (state.deadline && *state.deadline <= clock::now()) return fail(std::errc::timed_out);

    auto& reg  = it->second;
    auto& slot = state.op == poll_op::read ? reg.read : reg.write;
    if (slot != nullptr) return fail(std::errc::device_or_resource_busy);

    try
    {
        loop.arm(state.fd, interest(reg) | state.op);
    }
    catch (const std::system_error& err)
    {
        state.result = err.code().default_error_condition();
        return false;
    }

    slot           = &state;
    state.awaiting = awaiting;

    bool earliest = false;
    if (state.deadline)
    {
        state.timer = timers.emplace(*state.deadline, &state);
        earliest    = *state.timer == timers.begin();
    }

    // Once unlocked, the wait may complete and the coroutine resume (on another thread) at any moment: state must not
    // be touched again.
    lock.unlock();

    // run() may be waiting for a later deadline (or none at all).
    if (earliest) loop.interrupt();

    return true;
}

void scheduler::complete(waiter* state, std::error_condition result, completions& done) noexcept
{
    if (state->op != poll_op::none)
    {
        auto it = registrations.find(state->fd);
        if (it != registrations.end())
        {
            auto& slot = state->op == poll_op::read ? it->second.read : it->second.write;
            if (slot == state) slot = nullptr;
        }
    }

    if (state->watch)
    {
        watched.erase(*state->watch);
        state->watch.reset();
    }

    if (state->timer)
    {
        timers.erase(*state->timer);
        state->timer.reset();
    }

    state->result = result;
    done.push_back(state->awaiting);
}

void scheduler::rearm(handle fd, const registration& reg, completions& done) noexcept
{
    try
    {
        loop.arm(fd, interest(reg));
    }
    catch (const std::system_error& err)
    {
        // nothing would wake the remaining waiters, so fail them instead.
        auto result = err.code().default_error_condition();
        if (reg.read != nullptr) complete(reg.read, result, done);
        if (reg.write != nullptr) complete(reg.write, result, done);
    }
}

void scheduler::resume(const completions& done) noexcept
{
    for (auto awaiting : done)
    {
        if (!workers->resume(awaiting))
        {
            logger->warn("thread pool is not running; resuming task on the current thread");
            awaiting.resume();
        }
    }
}

void scheduler::run()
{
    if (running.exchange(true, std::memory_order::acq_rel)) throw std::logic_error{"scheduler is already running"};

    struct on_exit
    {
        explicit on_exit(std::atomic<bool>& running) noexcept
            : running{running}
        {}

        on_exit(const on_exit&)            = delete;
        on_exit& operator=(const on_exit&) = delete;
        on_exit(on_exit&&)                 = delete;
        on_exit& operator=(on_exit&&)      = delete;

        ~on_exit()
        {
            running.store(false, std::memory_order::release);
            running.notify_all();
        }

        std::atomic<bool>& running;
    } reset_running{running};

    std::array<ready_event, 64> events{};
    completions                 done;

    while (true)
    {
        std::optional<clock::duration> timeout;

        {
            std::scoped_lock lock{mu};

            // NOTE: the scope interrupts the loop once it has drained after being closed.
            if (stopping && scope.empty()) return;

            if (!timers.empty()) timeout = std::max(timers.begin()->first - clock::now(), clock::duration::zero());
        }

        auto num_ready = loop.wait(events, timeout);

        {
            std::scoped_lock lock{mu};
            dispatch(std::span{events}.first(num_ready), done);
            expire_timers(done);
        }

        resume(done);
        done.clear();
    }
}

void scheduler::dispatch(std::span<const ready_event> events, completions& done) noexcept
{
    for (const auto& event : events)
    {
        auto it = registrations.find(event.fd);
        if (it == registrations.end()) continue;

        auto& reg   = it->second;
        bool  fired = false;

        if (is_readable(event.ready) && reg.read != nullptr)
        {
            complete(reg.read, {}, done);
            fired = true;
        }

        if (is_writable(event.ready) && reg.write != nullptr)
        {
            complete(reg.write, {}, done);
            fired = true;
        }

        // The handle is disarmed once reported, so if another operation is still waiting, arm it again.
        // (Spurious events, with nothing waiting, are ignored: whatever is waiting has armed it since.)
        if (fired && interest(reg) != poll_op::none) rearm(event.fd, reg, done);
    }
}

void scheduler::expire_timers(completions& done) noexcept
{
    auto now       = clock::now();
    auto timed_out = std::make_error_condition(std::errc::timed_out);

    while (!timers.empty() && timers.begin()->first <= now)
    {
        auto* state = timers.begin()->second;
        auto  fd    = state->fd;
        auto  op    = state->op;

        // a sleep that's over isn't an error.
        const bool sleep = op == poll_op::none && state->word == nullptr;
        complete(state, sleep ? std::error_condition{} : timed_out, done);

        // stop watching for the operation that timed out.
        if (op != poll_op::none)
        {
            if (auto it = registrations.find(fd); it != registrations.end()) rearm(fd, it->second, done);
        }
    }
}

void scheduler::request_stop() noexcept
{
    completions done;

    {
        std::scoped_lock lock{mu};
        if (std::exchange(stopping, true)) return;

        auto canceled = std::make_error_condition(std::errc::operation_canceled);

        for (auto& [fd, reg] : registrations)
        {
            if (reg.read != nullptr) complete(reg.read, canceled, done);
            if (reg.write != nullptr) complete(reg.write, canceled, done);
        }

        // then waits for notifications, and (all that's left with a timer) sleeps.
        while (!watched.empty()) complete(watched.begin()->second, canceled, done);
        while (!timers.empty()) complete(timers.begin()->second, canceled, done);
    }

    scope.close();
    resume(done);

    // wake run(), so it notices once the scope has drained.
    loop.interrupt();
}

void scheduler::notify_one(const std::atomic<std::uint64_t>& word) noexcept { notify(word, false); }

void scheduler::notify_all(const std::atomic<std::uint64_t>& word) noexcept { notify(word, true); }

void scheduler::notify(const std::atomic<std::uint64_t>& word, bool all) noexcept
{
    completions done;

    {
        std::scoped_lock lock{mu};

        // NOTE: equal keys are kept in the order they were inserted, and lower_bound finds the first (find needn't).
        while (true)
        {
            auto it = watched.lower_bound(&word);
            if (it == watched.end() || it->first != &word) break;

            complete(it->second, {}, done);
            if (!all) break;
        }
    }

    resume(done);
}

bool scheduler::stop_requested() const noexcept
{
    std::scoped_lock lock{mu};
    return stopping;
}

void scheduler::shutdown() noexcept
{
    request_stop();
    scope.join_blocking();
    running.wait(true, std::memory_order::acquire);
}

}
