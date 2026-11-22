#pragma once

#include <cerrno>

#include <fcntl.h>

#include "io/io.hpp"

namespace net::detail
{

// make_nonblocking_cloexec makes fd non-blocking (the scheduler only says when a socket is ready, so its calls must
// never block) and keeps it from leaking into child processes. It returns the errno of what failed, or 0. (The socket
// and the listener differ in what they do about a failure.)
inline int make_nonblocking_cloexec(io::handle fd) noexcept
{
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        return errno;
    return 0;
}

}
