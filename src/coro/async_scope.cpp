#include "coro/async_scope.hpp"

#include <cstddef>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"

namespace
{

// awaited is a coroutine for a task without one of its own left to run (see async_scope::spawn()): it awaits task, so
// the task ends (or throws) as it would awaited anywhere.
net::coro::task<> awaited(net::coro::task<> task) { co_await std::move(task); }

}

namespace net::coro
{

// NOLINTNEXTLINE(bugprone-exception-escape): an exception thrown by the scope's exception handler ends the program.
void promise<void>::end_detached(std::coroutine_handle<promise> self) noexcept
{
    auto& scope = *self.promise().owner;

    if (auto error = std::exchange(self.promise().exception, nullptr); error != nullptr)
    {
        if (scope.on_exception) scope.on_exception(error);
        else std::terminate();
    }

    // the frame is destroyed before the scope is told the task has finished, so nothing of the task (e.g. what its
    // parameters own) outlives the scope.
    self.destroy();
    scope.finish();
}

bool async_scope::join_operation::await_suspend(std::coroutine_handle<> awaiting) noexcept
{
    std::scoped_lock lock{scope->mu};
    if (scope->count == 0) return false;

    scope->joiners.push_back(awaiting);
    return true;
}

async_scope::async_scope(exception_handler on_exception, drained_handler on_drained)
    : on_exception{std::move(on_exception)}
    , on_drained{std::move(on_drained)}
{}

async_scope::~async_scope()
{
    close();
    join_blocking();
}

bool async_scope::spawn(task<> task)
{
    auto handle = adopt(task);
    if (!handle) return false;

    // NOTE: the frame may be destroyed (on another thread) before resume() returns.
    handle.resume();

    return true;
}

bool async_scope::spawn_on(thread_pool& pool, task<> task)
{
    auto handle = adopt(task);
    if (!handle) return false;

    // NOTE: once it's queued, a worker may run it to its end (destroying its frame) before queue() returns.
    if (pool.queue(handle)) return true;

    // the pool is shutting down: the task never starts.
    handle.destroy();
    finish();
    return false;
}

std::coroutine_handle<promise<void>> async_scope::adopt(task<>& task)
{
    if (auto handle = task.get_handle(); !handle || handle.done()) task = awaited(std::move(task));

    {
        std::scoped_lock lock{mu};
        if (is_closed) return nullptr;
        ++count;
    }

    auto handle = std::exchange(task.handle, nullptr);
    handle.promise().detach(*this);
    return handle;
}

async_scope::lease async_scope::acquire() noexcept
{
    std::scoped_lock lock{mu};
    if (is_closed) return lease{};

    ++count;
    return lease{this};
}

// NOLINTNEXTLINE(bugprone-exception-escape): on_drained is checked before it's called.
void async_scope::close() noexcept
{
    std::scoped_lock lock{mu};
    if (std::exchange(is_closed, true)) return;

    if (count == 0 && on_drained) on_drained();
}

bool async_scope::closed() const noexcept
{
    std::scoped_lock lock{mu};
    return is_closed;
}

std::size_t async_scope::size() const noexcept
{
    std::scoped_lock lock{mu};
    return count;
}

void async_scope::join_blocking() noexcept
{
    std::unique_lock lock{mu};
    drained.wait(lock, [this] { return count == 0; });
}

// NOLINTNEXTLINE(bugprone-exception-escape): on_drained is checked before it's called.
void async_scope::finish() noexcept
{
    std::vector<std::coroutine_handle<>> to_resume;

    {
        // NOTE: everything is done under the lock, as once it is released, a joiner may destroy the scope.
        std::scoped_lock lock{mu};
        if (--count != 0) return;

        to_resume.swap(joiners);

        if (is_closed && on_drained) on_drained();
        drained.notify_all();
    }

    for (auto handle : to_resume) handle.resume();
}

}
