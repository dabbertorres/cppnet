#include "io/scheduler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <compare>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
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

// making is the call this thread is making, if any: so that a call that cancels itself doesn't wait for itself.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): a thread's own.
thread_local const net::io::scheduler::call_base* making = nullptr;

}

namespace net::io
{

scheduler::scheduler(std::shared_ptr<coro::thread_pool> workers, const std::shared_ptr<spdlog::logger>& logger)
    : scheduler{std::move(workers), options{}, logger}
{}

scheduler::scheduler(std::shared_ptr<coro::thread_pool>     workers,
                     options                                opts,
                     const std::shared_ptr<spdlog::logger>& logger)
    : workers{std::move(workers)}
    , blocking{std::make_unique<coro::thread_pool>(std::max<std::size_t>(opts.blocking_threads, 1))}
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
{
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer): once every member is, as workers may poll at once.
    workers_poll = this->workers->attach(this);
}

// NOTE: shutdown() throws if this is one of the pool's workers, which would wait for itself: that terminates.
scheduler::~scheduler() noexcept // NOLINT(bugprone-exception-escape)
{
    shutdown();
    if (workers_poll) workers->detach(this);
}

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

        auto canceled = std::make_error_code(std::errc::operation_canceled);
        if (it->second.read.waiting != nullptr) done.push_back(complete_handle_wait(sh, it->second.read, canceled));
        if (it->second.write.waiting != nullptr) done.push_back(complete_handle_wait(sh, it->second.write, canceled));

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
        state.result = std::make_error_code(std::errc::invalid_argument);
        return false;
    }

    return suspend_handle(state, awaiting);
}

bool scheduler::suspend_handle(waiter& state, std::coroutine_handle<> awaiting) noexcept
{
    const auto fail = [&](std::errc err)
    {
        state.result = std::make_error_code(err);
        return false; // don't suspend
    };

    auto&            sh = shard_for(state.fd);
    std::unique_lock lock{sh.mu};

    // NOTE: read under the shard's lock: request_stop() sets it before going through the shards, so either this sees
    //       it, or request_stop() sees this wait. Likewise canceled, which stop_wait() sets under the lock.
    if (state.canceled || stopping.load(std::memory_order::acquire)) return fail(std::errc::operation_canceled);

    auto it = sh.registrations.find(state.fd);
    if (it == sh.registrations.end()) return fail(std::errc::bad_file_descriptor);

    // NOTE: read under the lock, as limit_deadline() sets it: so either this sees its new limit, or it sees this wait.
    if (state.operation != nullptr) state.deadline = earliest(state.deadline, state.operation->limit);

    if (state.deadline && *state.deadline <= clock::now()) return fail(std::errc::timed_out);

    auto& slot = state.op == poll_op::read ? it->second.read : it->second.write;
    if (slot.waiting != nullptr) return fail(std::errc::device_or_resource_busy);

    // The handle became ready since the caller last found it wasn't (e.g. a read got EAGAIN), so it can try again
    // straight away: as handles are only reported when they become ready, waiting would miss it. An operation is tried
    // here, and waits if it would still have to (e.g. the event was spurious).
    if (std::exchange(slot.ready, false) && (state.operation == nullptr || state.operation->perform()))
    {
        state.result = {};
        return false;
    }

    slot.waiting   = &state;
    state.awaiting = awaiting;

    const auto deadline = state.deadline;
    if (deadline)
    {
        state.timer = sh.timers.add(*deadline, &state);
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
        state.result = std::make_error_code(err);
        return false; // don't suspend
    };

    std::unique_lock lock{timers_mu};

    if (state.canceled || stopping.load(std::memory_order::acquire)) return fail(std::errc::operation_canceled);

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
    if (deadline) state.timer = timers.add(*deadline, &state);

    lock.unlock();

    if (deadline) note_deadline(*deadline);
    return true;
}

std::coroutine_handle<> scheduler::complete_handle_wait(shard& sh, direction& slot, std::error_code result) noexcept
{
    auto* state   = std::exchange(slot.waiting, nullptr);
    state->result = result;

    if (state->timer)
    {
        sh.timers.remove(*state->timer);
        state->timer.reset();
        sh.update_earliest();
    }

    return state->awaiting;
}

std::coroutine_handle<> scheduler::complete_other(waiter* state, std::error_code result) noexcept
{
    if (state->watch)
    {
        watched.erase(*state->watch);
        state->watch.reset();
    }

    if (state->timer)
    {
        timers.remove(*state->timer);
        state->timer.reset();
    }

    state->result = result;
    return state->awaiting;
}

void scheduler::resume(const completions& done) noexcept
{
    if (done.empty() || workers->resume(done) != 0) return;

    logger->warn("thread pool is not running; resuming tasks on the current thread");
    for (auto awaiting : done) awaiting.resume();
}

void scheduler::resume(std::coroutine_handle<> awaiting) noexcept
{
    if (workers->resume(awaiting)) return;

    logger->warn("thread pool is not running; resuming a task on the current thread");
    awaiting.resume();
}

void scheduler::run()
{
    if (running.exchange(true, std::memory_order::acq_rel)) throw std::logic_error{"scheduler is already running"};

    util::defer on_exit{[this] noexcept
                        {
                            running.store(false, std::memory_order::release);
                            running.notify_all();
                        }};

    if (workers_poll)
    {
        stopping.wait(false, std::memory_order::acquire);
        scope.join_blocking();
        return;
    }

    completions done;
    while (true)
    {
        // NOTE: the scope interrupts the loop once it has drained after being closed.
        if (stopping.load(std::memory_order::acquire) && scope.empty()) return;

        poll(true, done);
        resume(done);
        done.clear();
    }
}

void scheduler::poll(bool block, completions& done) noexcept
{
    std::array<ready_event, 64>    events{};
    std::optional<clock::duration> timeout = clock::duration::zero();

    if (block)
    {
        // A wait with an earlier deadline, from after next_deadline() looked, interrupts the loop: either it's in time
        // for the second look, or it sees what sleeping_until was set to in between (as both use sequential
        // consistency), and is earlier (see note_deadline()).
        auto next = next_deadline();
        sleeping_until.store(next);
        next = std::min(next, next_deadline());

        timeout.reset();
        if (next != std::numeric_limits<clock::rep>::max())
            timeout = std::max(clock::time_point{clock::duration{next}} - clock::now(), clock::duration::zero());
    }

    std::size_t num_ready = 0;
    try
    {
        num_ready = loop.wait(events, timeout);
    }
    catch (const std::exception& err)
    {
        logger->error("failed to wait for I/O: {}", err.what());
    }

    if (block) sleeping_until.store(std::numeric_limits<clock::rep>::max());

    dispatch(std::span{events}.first(num_ready), done);
    // NOTE: calls are made once expire_timers() has released the locks, as they may request stop (whose callbacks
    //       take them).
    if (expire_timers(done)) make_calls();
}

void scheduler::wake() noexcept { loop.interrupt(); }

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
        if (is_readable(event.ready)) ready_for(sh, it->second.read, done);
        if (is_writable(event.ready)) ready_for(sh, it->second.write, done);
    }
}

void scheduler::ready_for(shard& sh, direction& slot, completions& done) noexcept
{
    auto* state = slot.waiting;
    if (state == nullptr)
    {
        slot.ready = true;
        return;
    }

    if (state->operation != nullptr)
    {
        const auto deadline = state->deadline;
        if (!state->operation->perform())
        {
            // it would have to wait again (e.g. a write that wrote some, but not all): so it does, until its deadline,
            // which it may have moved.
            if (state->deadline != deadline) retime(sh, *state);
            return;
        }
    }

    done.push_back(complete_handle_wait(sh, slot, {}));
}

void scheduler::retime(shard& sh, waiter& state) noexcept
{
    if (state.timer)
    {
        sh.timers.remove(*state.timer);
        state.timer.reset();
    }

    if (state.deadline)
    {
        state.timer = sh.timers.add(*state.deadline, &state);
        note_deadline(*state.deadline);
    }

    sh.update_earliest();
}

bool scheduler::handle_operation::begin(scheduler&                       sched,
                                        handle                           fd,
                                        poll_op                          op,
                                        std::optional<clock::time_point> deadline,
                                        std::coroutine_handle<>          awaiting,
                                        const std::stop_token&           stop) noexcept
{
    state        = waiter{.fd = fd, .op = op, .deadline = deadline, .operation = this};
    own_deadline = deadline;
    this->sched  = &sched;

    // NOTE: registered before the wait takes the shard's lock (see stoppable_wait_operation), and after state is reset
    //       (above), which would forget that a callback that ran at once canceled it.
    if (stop.stop_possible()) on_stop.emplace(stop, canceler{.sched = &sched, .state = &state});
    return sched.suspend(state, awaiting);
}

void scheduler::handle_operation::limit_deadline(scheduler&                       sched,
                                                 handle                           fd,
                                                 std::optional<clock::time_point> new_limit) noexcept
{
    auto&            sh = sched.shard_for(fd);
    std::scoped_lock lock{sh.mu};

    limit = new_limit;

    // the wait in progress, if this operation's is: its deadline (and the rest of state) is only touched under the lock
    // while it waits.
    auto it = sh.registrations.find(fd);
    if (it == sh.registrations.end()) return;
    if (it->second.read.waiting != &state && it->second.write.waiting != &state) return;

    state.deadline = earliest(own_deadline, limit);
    sched.retime(sh, state);
}

void scheduler::handle_operation::cancel_wait() noexcept
{
    if (sched != nullptr) sched->cancel(state);
    end_wait();
}

void scheduler::cancel(waiter& state) noexcept
{
    auto&            sh = shard_for(state.fd);
    std::scoped_lock lock{sh.mu};

    auto it = sh.registrations.find(state.fd);
    if (it == sh.registrations.end()) return;

    // it may have finished (or been cancelled by deregistering the handle) already.
    auto& slot = state.op == poll_op::read ? it->second.read : it->second.write;
    if (slot.waiting != &state) return;

    slot.waiting = nullptr;
    if (state.timer)
    {
        sh.timers.remove(*state.timer);
        state.timer.reset();
        sh.update_earliest();
    }
}

void scheduler::stop_wait(waiter& state) noexcept
{
    const auto canceled = std::make_error_code(std::errc::operation_canceled);

    std::coroutine_handle<> awaiting{nullptr};

    // NOTE: op and fd don't change while the callback is registered, so they're read without the lock.
    if (state.op == poll_op::none)
    {
        std::scoped_lock lock{timers_mu};

        // it's waiting while it's watching its word, or has a timer (complete_other() ends both).
        if (state.watch || state.timer) awaiting = complete_other(&state, canceled);
        else state.canceled = true;
    }
    else
    {
        auto&            sh = shard_for(state.fd);
        std::scoped_lock lock{sh.mu};

        auto it = sh.registrations.find(state.fd);
        if (it != sh.registrations.end())
        {
            auto& slot = state.op == poll_op::read ? it->second.read : it->second.write;
            if (slot.waiting == &state) awaiting = complete_handle_wait(sh, slot, canceled);
        }

        // it hasn't begun waiting (or has finished, when this does nothing).
        if (!awaiting) state.canceled = true;
    }

    // NOTE: on the pool, never here: whoever requested stop may hold a lock the coroutine takes. state mustn't be
    //       touched from here on, as the coroutine may already have finished with it.
    if (awaiting) resume(awaiting);
}

bool scheduler::expire_timers(completions& done) noexcept
{
    const auto now       = clock::now();
    const auto timed_out = std::make_error_code(std::errc::timed_out);

    for (auto& sh : shards)
    {
        if (sh.earliest.load() > now.time_since_epoch().count()) continue;

        std::scoped_lock lock{sh.mu};
        while (!sh.timers.empty() && sh.timers.earliest() <= now)
        {
            const auto* state = sh.timers.begin()->second;

            // NOTE: a waiter with a timer is always in its handle's slot, as both change under this lock.
            auto it = sh.registrations.find(state->fd);
            if (it == sh.registrations.end())
            {
                sh.timers.remove(sh.timers.begin());
                sh.update_earliest();
                continue;
            }

            done.push_back(
                complete_handle_wait(sh, state->op == poll_op::read ? it->second.read : it->second.write, timed_out));
        }
    }

    std::scoped_lock lock{timers_mu};
    while (!timers.empty() && timers.earliest() <= now)
    {
        auto* state = timers.begin()->second;

        // a sleep that's over isn't an error.
        done.push_back(complete_other(state, state->word == nullptr ? std::error_code{} : timed_out));
    }

    return !calls.empty() && calls.earliest() <= now;
}

clock::rep scheduler::next_deadline() const noexcept
{
    auto next = std::numeric_limits<clock::rep>::max();
    for (const auto& sh : shards) next = std::min(next, sh.earliest.load());

    std::scoped_lock lock{timers_mu};
    if (!timers.empty()) next = std::min(next, timers.earliest().time_since_epoch().count());
    if (!calls.empty()) next = std::min(next, calls.earliest().time_since_epoch().count());
    return next;
}

void scheduler::schedule_call(call_base& call, clock::time_point deadline) noexcept
{
    {
        std::scoped_lock lock{timers_mu};

        // NOTE: read under timers_mu: request_stop() sets it before dropping the calls under it, so either this sees
        // it,
        //       or request_stop() sees this call.
        if (stopping.load(std::memory_order::acquire)) return;

        call.timer  = calls.add(deadline, &call);
        call.status = call_base::stage::pending;
    }

    // run() may be waiting for a later deadline (or none at all).
    note_deadline(deadline);
}

void scheduler::make_calls() noexcept
{
    const auto now = clock::now();

    while (true)
    {
        call_base* due = nullptr;

        {
            std::scoped_lock lock{timers_mu};
            if (calls.empty() || calls.earliest() > now) return;

            due = calls.begin()->second;
            calls.remove(calls.begin());
            due->timer.reset();
            due->status = call_base::stage::calling;
        }

        making = due;
        due->invoke(*due);
        making = nullptr;

        {
            std::scoped_lock lock{timers_mu};
            due->status = call_base::stage::called;

            // NOTE: under the lock, so that a cancel() waiting for it can't return (and the call go) before this is
            // done
            //       with it.
            calls_done.notify_all();
        }
    }
}

bool scheduler::call_base::cancel() noexcept
{
    std::unique_lock lock{sched->timers_mu};

    if (status == stage::pending && timer)
    {
        sched->calls.remove(*timer);
        timer.reset();
        status = stage::idle;
        return true;
    }

    // being made on another thread: so that what it refers to may go once this returns.
    if (status == stage::calling && making != this)
        sched->calls_done.wait(lock, [this] { return status != stage::calling; });

    return false;
}

void scheduler::note_deadline(clock::time_point deadline) noexcept
{
    if (deadline.time_since_epoch().count() < sleeping_until.load()) loop.interrupt();
}

void scheduler::request_stop() noexcept
{
    if (stopping.exchange(true, std::memory_order::acq_rel)) return;
    stopping.notify_all(); // for run()

    completions done;
    auto        canceled = std::make_error_code(std::errc::operation_canceled);

    for (auto& sh : shards)
    {
        std::scoped_lock lock{sh.mu};

        for (auto& [fd, reg] : sh.registrations)
        {
            if (reg.read.waiting != nullptr) done.push_back(complete_handle_wait(sh, reg.read, canceled));
            if (reg.write.waiting != nullptr) done.push_back(complete_handle_wait(sh, reg.write, canceled));
        }
    }

    {
        std::scoped_lock lock{timers_mu};

        // then waits for notifications, and (all that's left with a timer) sleeps.
        while (!watched.empty()) done.push_back(complete_other(watched.begin()->second, canceled));
        while (!timers.empty()) done.push_back(complete_other(timers.begin()->second, canceled));

        // and calls, which aren't made.
        while (!calls.empty())
        {
            auto* call = calls.begin()->second;
            calls.remove(calls.begin());
            call->timer.reset();
            call->status = call_base::stage::idle;
        }
    }

    scope.close();
    resume(done);

    // NOTE: holding none of the locks, as its callbacks may take them (e.g. stop_wait()'s, which find the waits
    //       canceled already).
    stopper.request_stop();

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

            done.push_back(complete_other(it->second, {}));
            if (!all) break;
        }
    }

    resume(done);
}

bool scheduler::stop_requested() const noexcept { return stopping.load(std::memory_order::acquire); }

void scheduler::shutdown()
{
    // it would wait for the task calling it to finish, which it never would.
    if (workers->on_worker())
        throw std::logic_error{"scheduler::shutdown() was called from one of its workers: use request_stop()"};

    request_stop();
    scope.join_blocking();
    running.wait(true, std::memory_order::acquire);
}

}
