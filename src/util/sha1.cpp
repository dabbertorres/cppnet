#include "util/sha1.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace net::util
{

sha1& sha1::update(std::span<const std::byte> data) noexcept
{
    length += data.size();

    if (buffered > 0)
    {
        const auto take = std::min(data.size(), block_size - buffered);
        std::ranges::copy(data.first(take), buffer.begin() + static_cast<std::ptrdiff_t>(buffered));
        buffered += take;
        data = data.subspan(take);
        if (buffered < block_size) return *this;

        compress(buffer.data());
        buffered = 0;
    }

    // whole blocks straight from data
    for (; data.size() >= block_size; data = data.subspan(block_size)) compress(data.data());

    std::ranges::copy(data, buffer.begin());
    buffered = data.size();
    return *this;
}

sha1::digest sha1::finish() noexcept
{
    const std::uint64_t bits = length * 8;

    // 0x80, zeros up to 56 bytes into a block (a block more if there isn't room), then the length in bits, big-endian
    std::array<std::byte, block_size + 8> padding{};
    padding[0]            = std::byte{0x80};
    const std::size_t pad = buffered < block_size - 8 ? block_size - 8 - buffered : (2 * block_size) - 8 - buffered;
    for (std::size_t i = 0; i < 8; ++i) padding[pad + i] = static_cast<std::byte>(bits >> (56 - (8 * i)));
    update(std::span{padding}.first(pad + 8));

    digest out{};
    for (std::size_t i = 0; i < state.size(); ++i)
    {
        for (std::size_t j = 0; j < 4; ++j) out[(4 * i) + j] = static_cast<std::byte>(state[i] >> (24 - (8 * j)));
    }

    *this = sha1{};
    return out;
}

void sha1::compress(const std::byte* block) noexcept
{
    // the message schedule, kept to 16 words and computed as it's used
    std::array<std::uint32_t, 16> w{};
    for (std::size_t i = 0; i < w.size(); ++i)
    {
        w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) | (static_cast<std::uint32_t>(block[(4 * i) + 1]) << 16)
             | (static_cast<std::uint32_t>(block[(4 * i) + 2]) << 8) | static_cast<std::uint32_t>(block[(4 * i) + 3]);
    }

    auto [a, b, c, d, e] = state;
    for (std::size_t t = 0; t < 80; ++t)
    {
        if (t >= 16)
        {
            w[t % 16] = std::rotl(w[(t - 3) % 16] ^ w[(t - 8) % 16] ^ w[(t - 14) % 16] ^ w[t % 16], 1);
        }

        std::uint32_t f = 0;
        std::uint32_t k = 0;
        if (t < 20)
        {
            f = (b & c) | (~b & d);
            k = 0x5a82'7999;
        }
        else if (t < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ed9'eba1;
        }
        else if (t < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1b'bcdc;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xca62'c1d6;
        }

        const std::uint32_t temp = std::rotl(a, 5) + f + e + k + w[t % 16];
        e                        = d;
        d                        = c;
        c                        = std::rotl(b, 30);
        b                        = a;
        a                        = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

}
