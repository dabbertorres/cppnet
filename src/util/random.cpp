#include "util/random.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include "config.hpp" // IWYU pragma: keep (for the platform macros)

#ifdef NET_IS_LINUX
#    include <cerrno>
#    include <exception>

#    include <sys/random.h>
#else
#    include <cstdlib>
#endif

namespace
{

constexpr std::size_t batch_size = 256;

// fill fills out from the OS's randomness.
void fill(std::span<std::byte> out) noexcept
{
#ifdef NET_IS_LINUX
    while (!out.empty())
    {
        const auto got = ::getrandom(out.data(), out.size(), 0);
        if (got < 0)
        {
            if (errno == EINTR) continue;
            // NOTE: only kernels older than 3.17 lack it; glibc's arc4random() aborts too, without randomness.
            std::terminate();
        }
        out = out.subspan(static_cast<std::size_t>(got));
    }
#else
    ::arc4random_buf(out.data(), out.size());
#endif
}

}

namespace net::util
{

void random_bytes(std::span<std::byte> out) noexcept
{
    if (out.size() >= batch_size)
    {
        fill(out);
        return;
    }

    // the batch is used from its end: left is how much of its start hasn't been given out
    thread_local std::array<std::byte, batch_size> batch{};
    thread_local std::size_t                       left = 0;

    while (!out.empty())
    {
        if (left == 0)
        {
            fill(batch);
            left = batch.size();
        }

        const auto take = std::min(out.size(), left);
        left -= take;
        std::ranges::copy(std::span{batch}.subspan(left, take), out.begin());
        out = out.subspan(take);
    }
}

}
