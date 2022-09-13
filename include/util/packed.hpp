#pragma once

#if defined(__clang__)
#    define NET_PACKED __attribute__((packed))
#elif defined(__GNUC__) || defined(__GNUG__)
#    define NET_PACKED __attribute__((packed))
#elif defined(_MSC_VER)
#    error "c'mon visual C++, get with the program"
#else
#    error "could not determine compiler type"
#endif
