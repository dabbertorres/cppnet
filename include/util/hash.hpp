#pragma once

#include <bit>
#include <cstddef>
#include <functional>
#include <limits>

namespace net::util::detail
{

constexpr std::size_t xorshift(std::size_t n, int i) { return n ^ (n >> i); }

constexpr std::size_t distribute(std::size_t n)
{
    std::size_t p          = 0;
    std::size_t c          = 0;
    int         shift_size = 0;

    if constexpr (sizeof(n) == 4)
    {
        p          = 0x5555'5555ul; // pattern of alternating 0 and 1
        c          = 3423571495ul;  // random uneven integer constant
        shift_size = 16;
    }
    else if constexpr (sizeof(n) == 8)
    {
        p          = 0x5555'5555'5555'5555ull; // pattern of alternating 0 and 1
        c          = 17316035218449499591ull;  // random uneven integer constant
        shift_size = 32;
    }

    return c * xorshift(p * xorshift(n, shift_size), shift_size);
}

template<typename T>
constexpr std::size_t hash_combine(std::size_t seed, const T& v)
{
    constexpr std::size_t rotate_by = std::numeric_limits<std::size_t>::digits / 3;

    return std::rotl(seed, rotate_by) ^ distribute(std::hash<T>{}(v));
}

}
