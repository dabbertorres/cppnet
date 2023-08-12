#pragma once

#if defined(__linux__)

#    define NET_HAS_EPOLL 1

// clang-format off
#elif (defined(__APPLE__) && defined(__MACH__)) \
    || defined(__FreeBSD__) \
    || defined(__NetBSD__) \
    || defined(__OpenBSD__) \
    || defined(__bsdi__) \
    || defined(__DragonFly__)
// clang-format on

#    define NET_HAS_KQUEUE 1

#elif defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__)

#    define NET_HAS_COMPLETION_PORTS 1

#else

#    error "unsupported OS target"

#endif
