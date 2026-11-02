#pragma once

#include <stdexcept>
#include <utility>

#include "coro/task.hpp"

namespace test
{

// sync_wait runs a task that completes without suspending (e.g. one that only uses string readers/writers), and
// returns its result.
//
// Tasks are lazy, so they must be resumed before their result is available (unless they're ready tasks, which have no
// coroutine). An operation (see net::coro::operation_task) is awaited by a coroutine of sync_wait's: it must finish
// when it's started (e.g. an adapter's whose inner tasks do), as nothing here would finish it later (as e.g. the
// scheduler would).
template<typename T>
T sync_wait(net::coro::task<T>&& task)
{
    if (task.finished_without_coroutine()) return std::move(task).result();

    if (!task.get_handle())
    {
        auto awaiting = [](net::coro::task<T> t) -> net::coro::task<T>
        { co_return co_await std::move(t); }(std::move(task));
        return sync_wait(std::move(awaiting));
    }

    auto handle = task.get_handle();
    handle.resume();
    if (!handle.done()) throw std::logic_error{"sync_wait: task suspended, but only synchronous tasks are supported"};

    return std::move(task).result();
}

}
