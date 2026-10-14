#include "coro/async_scope.hpp"

#include <coroutine>
#include <cstddef>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

#include "coro/task.hpp"

namespace net::coro
{

bool async_scope::join_operation::await_suspend(std::coroutine_handle<> awaiting) noexcept
{
    std::lock_guard lock{scope->mu};
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
    {
        std::lock_guard lock{mu};
        if (is_closed) return false;
        ++count;
    }

    detached owner{};

    try
    {
        owner = run(this, std::move(task));
    }
    catch (...)
    {
        // the coroutine frame couldn't be allocated, so the task never started.
        finish();
        throw;
    }

    // NOTE: the frame may be destroyed (on another thread) before resume() returns.
    owner.handle.resume();

    return true;
}

async_scope::detached async_scope::run(async_scope* scope, task<> task)
{
    {
        // moved into a nested scope, so the task's frame is destroyed before the scope is told it is finished.
        auto owned = std::move(task);

        try
        {
            co_await owned;
        }
        catch (...)
        {
            if (scope->on_exception) scope->on_exception(std::current_exception());
            else std::terminate();
        }
    }
}

void async_scope::close() noexcept
{
    std::lock_guard lock{mu};
    if (std::exchange(is_closed, true)) return;

    if (count == 0 && on_drained) on_drained();
}

bool async_scope::closed() const noexcept
{
    std::lock_guard lock{mu};
    return is_closed;
}

std::size_t async_scope::size() const noexcept
{
    std::lock_guard lock{mu};
    return count;
}

void async_scope::join_blocking() noexcept
{
    std::unique_lock lock{mu};
    drained.wait(lock, [this] { return count == 0; });
}

void async_scope::finish() noexcept
{
    std::vector<std::coroutine_handle<>> to_resume;

    {
        // NOTE: everything is done under the lock, as once it is released, a joiner may destroy the scope.
        std::lock_guard lock{mu};
        if (--count != 0) return;

        to_resume.swap(joiners);

        if (is_closed && on_drained) on_drained();
        drained.notify_all();
    }

    for (auto handle : to_resume) handle.resume();
}

}
