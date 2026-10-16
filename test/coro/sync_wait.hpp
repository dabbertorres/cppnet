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
// coroutine).
template<typename T>
T sync_wait(net::coro::task<T>&& task)
{
    if (auto handle = task.get_handle())
    {
        handle.resume();
        if (!handle.done())
            throw std::logic_error{"sync_wait: task suspended, but only synchronous tasks are supported"};
    }

    return std::move(task).result();
}

}
