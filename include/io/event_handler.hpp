#pragma once

#include <concepts>
#include <coroutine>

#include "io/io.hpp"
#include "io/poll.hpp"

namespace net::io::detail
{

template<typename T>
concept EventHandler = std::invocable<T, std::coroutine_handle<promise>, io::result>;

}
