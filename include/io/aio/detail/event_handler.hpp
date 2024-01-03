#pragma once

#include <concepts>
#include <coroutine>

#include "io/io.hpp"

#include "io/aio/poll.hpp"

namespace net::io::aio::detail
{

template<typename T>
concept EventHandler = std::invocable<T, std::coroutine_handle<promise>, io::result>;

}
