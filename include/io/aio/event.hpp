#pragma once

#include <coroutine>

#include "io/io.hpp"

#include "io/aio/poll.hpp"

namespace net::io::aio::detail
{

struct event
{
    std::coroutine_handle<promise> handle;
    io::result                     result;
};

}
