#pragma once

#include <array>
#include <cstddef>

namespace net::util
{

// fnv1a implements the 64-bit variant of FNV-1a.
template<std::size_t N>
constexpr std::size_t fnv1a(std::array<char, N> s) noexcept
{
    // https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function

    constexpr std::size_t prime = 0x00000100000001b3;
    constexpr std::size_t basis = 0xcbf29ce484222325;

    std::size_t hash = basis;

    for (auto c : s)
    {
        hash ^= c;
        hash *= prime;
    }

    return hash;
}

}
