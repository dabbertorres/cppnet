#include "io/scheduler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <cstdint>
#include <exception>
#include <limits>
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
#include "util/defer.hpp"

namespace
{

net::coro::task<> start_on(net::coro::thread_pool* workers, net::coro::task<> task)
{
    co_await workers->schedule();
    co_await task;
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
    auto&            sh = shard_for(fd);
    std::scoped_lock lock{sh.mu};

    if (sh.registrations.contains(fd)) return;

    // NOTE: under the shard's lock, so that an event for the handle (which may come straight away) finds it.
    loop.add(fd);
    sh.registrations.emplace(fd, registration{});
}

void scheduler::deregister_handle(handle fd) noexcept
{
    completions done;

    {
        auto&            sh = shard_for(fd);
        std::scoped_lock lock{sh.mu};

        auto it = sh.registrations.find(fd);
        if (it == sh.registrations.end()) return;

        auto canceled = std::make_error_condition(std::errc::operation_canceled);
        if (it->second.read.waiting != nullptr) complete_handle_wait(sh, it->second.read, canceled, done);
        if (it->second.write.waiting != nullptr) complete_handle_wait(sh, it->second.write, canceled, done);

        sh.registrations.erase(it);
        loop.remove(fd);
    }

    resume(done);
}

bool scheduler::spawn(coro::task<> task) { return scope.spawn(start_on(workers.get(), std::move(task))); }

bool scheduler::suspend(waiter& state, std::coroutine_handle<> awaiting) noexcept
{
    if (state.op == poll_op::none) return suspend_other(state, awaiting);
    if (state.op != poll_op::read && state.op != poll_op::write)
    {
        state.result = std::make_error_condition(std::errc::invalid_argument);
        return false;
    }

    return suspend_handle(state, awaiting);
}

bool scheduler::suspend_handle(waiter& state, std::coroutine_handle<> awaiting) noexcept
{
    const auto fail = [&](std::errc err)
    {
        state.result = std::make_error_condition(err);
        return false; // don't suspend
    };

    auto&            sh = shard_for(state.fd);
    std::unique_lock lock{sh.mu};

    // NOTE: read under the shard's lock: request_stop() sets it before going through the shards, so either this sees
    //       it, or request_stop() sees this wait.
    if (stopping.load(std::memory_order::acquire)) return fail(std::errc::operation_canceled);

    auto it = sh.registrations.find(state.fd);
    if (it == sh.registrations.end()) return fail(std::errc::bad_file_descriptor);

    if (state.deadline && *state.deadline <= clock::now()) return fail(std::errc::timed_out);

    auto& slot = state.op == poll_op::read ? it->second.read : it->second.write;
    if (slot.waiting != nullptr) return fail(std::errc::device_or_resource_busy);

    // The handle became ready since the caller last found it wasn't (e.g. a read got EAGAIN), so it can try again
    // straight away: as handles are only reported when they become ready, waiting would miss it.
    if (std::exchange(slot.ready, false))
    {
        state.result = {};
        return false;
    }

    slot.waiting   = &state;
    state.awaiting = awaiting;

    const auto deadline = state.deadline;
    if (deadline)
    {
        state.timer = sh.timers.emplace(*deadline, &state);
        sh.update_earliest();
    }

    // Once unlocked, the wait may complete and the coroutine resume (on another thread) at any moment: state must not
    // be touched again.
    lock.unlock();

    // run() may be waiting for a later deadline (or none at all).
    if (deadline) note_deadline(*deadline);

    return true;
}

bool scheduler::suspend_other(waiter& state, std::coroutine_handle<> awaiting) noexcept
{
    const auto fail = [&](std::errc err)
    {
        state.result = std::make_error_condition(err);
        return false; // don't suspend
    };

    std::unique_lock lock{timers_mu};

    if (stopping.load(std::memory_order::acquire)) return fail(std::errc::operation_canceled);

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

    const auto deadline = state.deadline;
    if (deadline) state.timer = timers.emplace(*deadline, &state);

    lock.unlock();

    if (deadline) note_deadline(*deadline);
    return true;
}

void scheduler::complete_handle_wait(shard&               sh,
                                     direction&           slot,
                                     std::error_condition result,
                                     completions&         done) noexcept
{
    auto* state   = std::exchange(slot.waiting, nullptr);
    state->result = result;

    if (state->timer)
    {
        sh.timers.erase(*state->timer);
        state->timer.reset();
        sh.update_earliest();
    }

    done.push_back(state->awaiting);
}

void scheduler::complete_other(waiter* state, std::error_condition result, completions& done) noexcept
{
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

void scheduler::resume(const completions& done) noexcept
{
    if (done.empty() || workers->resume(done) != 0) return;

    logger->warn("thread pool is not running; resuming tasks on the current thread");
    for (auto awaiting : done) awaiting.resume();
}

void scheduler::run()
{
    if (running.exchange(true, std::memory_order::acq_rel)) throw std::logic_error{"scheduler is already running"};

    util::defer on_exit{[this] noexcept
                        {
                            running.store(false, std::memory_order::release);
                            running.notify_all();
                        }};

    std::array<ready_event, 64> events{};
    std::size_t                 num_ready = 0;
    completions                 done;

    while (true)
    {
        dispatch(std::span{events}.first(num_ready), done);
        expire_timers(done);

        // NOTE: the scope interrupts the loop once it has drained after being closed.
        const bool finished = stopping.load(std::memory_order::acquire) && scope.empty();

        // A wait with an earlier deadline, from after next_deadline() looked, interrupts the loop: either it's in time
        // for the second look, or it sees what sleeping_until was set to in between (as both use sequential
        // consistency), and is earlier (see note_deadline()).
        auto next = next_deadline();
        sleeping_until.store(next);
        next = std::min(next, next_deadline());

        resume(done);
        done.clear();

        if (finished) return;

        std::optional<clock::duration> timeout;
        if (next != std::numeric_limits<clock::rep>::max())
            timeout = std::max(clock::time_point{clock::duration{next}} - clock::now(), clock::duration::zero());

        num_ready = loop.wait(events, timeout);
    }
}

void scheduler::dispatch(std::span<const ready_event> events, completions& done) noexcept
{
    for (const auto& event : events)
    {
        auto&            sh = shard_for(event.fd);
        std::scoped_lock lock{sh.mu};

        auto it = sh.registrations.find(event.fd);
        if (it == sh.registrations.end()) continue;

        // a waiter continues; otherwise, the next wait doesn't need to (see suspend_handle). Spurious events (e.g. for
        // a handle closed since, whose number was reused) only make a caller try its read or write once more.
        const auto ready = [&](direction& slot)
        {
            if (slot.waiting != nullptr) complete_handle_wait(sh, slot, {}, done);
            else slot.ready = true;
        };

        if (is_readable(event.ready)) ready(it->second.read);
        if (is_writable(event.ready)) ready(it->second.write);
    }
}

void scheduler::expire_timers(completions& done) noexcept
{
    const auto now       = clock::now();
    const auto timed_out = std::make_error_condition(std::errc::timed_out);

    for (auto& sh : shards)
    {
        if (sh.earliest.load() > now.time_since_epoch().count()) continue;

        std::scoped_lock lock{sh.mu};
        while (!sh.timers.empty() && sh.timers.begin()->first <= now)
        {
            const auto* state = sh.timers.begin()->second;

            // NOTE: a waiter with a timer is always in its handle's slot, as both change under this lock.
            auto it = sh.registrations.find(state->fd);
            if (it == sh.registrations.end())
            {
                sh.timers.erase(sh.timers.begin());
                sh.update_earliest();
                continue;
            }

            complete_handle_wait(sh, state->op == poll_op::read ? it->second.read : it->second.write, timed_out, done);
        }
    }

    std::scoped_lock lock{timers_mu};
    while (!timers.empty() && timers.begin()->first <= now)
    {
        auto* state = timers.begin()->second;

        // a sleep that's over isn't an error.
        complete_other(state, state->word == nullptr ? std::error_condition{} : timed_out, done);
    }
}

clock::rep scheduler::next_deadline() const noexcept
{
    auto next = std::numeric_limits<clock::rep>::max();
    for (const auto& sh : shards) next = std::min(next, sh.earliest.load());

    std::scoped_lock lock{timers_mu};
    if (!timers.empty()) next = std::min(next, timers.begin()->first.time_since_epoch().count());
    return next;
}

void scheduler::note_deadline(clock::time_point deadline) noexcept
{
    if (deadline.time_since_epoch().count() < sleeping_until.load()) loop.interrupt();
}

void scheduler::request_stop() noexcept
{
    if (stopping.exchange(true, std::memory_order::acq_rel)) return;

    completions done;
    auto        canceled = std::make_error_condition(std::errc::operation_canceled);

    for (auto& sh : shards)
    {
        std::scoped_lock lock{sh.mu};

        for (auto& [fd, reg] : sh.registrations)
        {
            if (reg.read.waiting != nullptr) complete_handle_wait(sh, reg.read, canceled, done);
            if (reg.write.waiting != nullptr) complete_handle_wait(sh, reg.write, canceled, done);
        }
    }

    {
        std::scoped_lock lock{timers_mu};

        // then waits for notifications, and (all that's left with a timer) sleeps.
        while (!watched.empty()) complete_other(watched.begin()->second, canceled, done);
        while (!timers.empty()) complete_other(timers.begin()->second, canceled, done);
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
        std::scoped_lock lock{timers_mu};

        // NOTE: equal keys are kept in the order they were inserted, and lower_bound finds the first (find needn't).
        while (true)
        {
            auto it = watched.lower_bound(&word);
            if (it == watched.end() || it->first != &word) break;

            complete_other(it->second, {}, done);
            if (!all) break;
        }
    }

    resume(done);
}

bool scheduler::stop_requested() const noexcept { return stopping.load(std::memory_order::acquire); }

void scheduler::shutdown() noexcept
{
    request_stop();
    scope.join_blocking();
    running.wait(true, std::memory_order::acquire);
}

}
