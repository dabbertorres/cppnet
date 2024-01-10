#pragma once

#include <coroutine>

#include "io/io.hpp"
#include "io/poll.hpp"

namespace net::io::detail
{

struct event
{
    std::coroutine_handle<promise> handle;
    io::result                     result;
};

}
