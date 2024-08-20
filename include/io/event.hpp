#pragma once

#include <coroutine>

#include "coro/task.hpp"
#include "io/io.hpp"

namespace net::io
{

using promise = coro::promise<result>;

struct event
{
    std::coroutine_handle<promise> handle;
    struct result                  result;
};

}
