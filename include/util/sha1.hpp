#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace net::util
{

// sha1 is SHA-1 (FIPS 180-4, RFC 3174), fed incrementally: for WebSockets' Sec-WebSocket-Accept (RFC 6455 4.2.2),
// which uses it only to show that the server read the key, not for security. SHA-1 isn't collision resistant: don't
// use it where that matters.
class sha1
{
public:
    static constexpr std::size_t digest_size = 20;
    static constexpr std::size_t block_size  = 64;

    using digest = std::array<std::byte, digest_size>;

    // update hashes data after what came before, in any pieces.
    sha1& update(std::span<const std::byte> data) noexcept;
    sha1& update(std::string_view text) noexcept { return update(std::as_bytes(std::span{text})); }

    // finish pads what was fed and returns its digest, leaving the hasher as a new one.
    [[nodiscard]] digest finish() noexcept;

private:
    static constexpr std::array<std::uint32_t, 5> initial{
        0x6745'2301, 0xefcd'ab89, 0x98ba'dcfe, 0x1032'5476, 0xc3d2'e1f0};

    void compress(const std::byte* block) noexcept;

    std::array<std::uint32_t, 5>      state = initial;
    std::array<std::byte, block_size> buffer{};
    std::size_t                       buffered = 0; // bytes of buffer holding a block's start
    std::uint64_t                     length   = 0; // bytes fed altogether
};

}
