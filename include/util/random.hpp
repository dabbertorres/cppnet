#pragma once

#include <cstddef>
#include <span>

namespace net::util
{

// random_bytes fills out from the OS's randomness (getrandom() on Linux, arc4random_buf() elsewhere): e.g. DNS query
// IDs, and WebSocket keys and masking keys. Spans smaller than a batch (256 bytes) are taken from a batch per thread,
// so small draws don't each make a system call; larger ones are filled directly.
void random_bytes(std::span<std::byte> out) noexcept;

}
