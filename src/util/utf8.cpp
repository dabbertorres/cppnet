#include "util/utf8.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace
{

constexpr std::size_t word = sizeof(std::uint64_t);

// ascii_word is whether the word bytes at p are all ASCII.
bool ascii_word(const std::byte* p) noexcept
{
    std::uint64_t w = 0;
    std::memcpy(&w, p, word);
    return (w & 0x8080'8080'8080'8080) == 0;
}

bool is_ascii(std::byte b) noexcept { return (b & std::byte{0x80}) == std::byte{0}; }

}

namespace net::util
{

bool utf8_validator::feed(std::span<const std::byte> bytes) noexcept
{
    std::size_t i = 0;
    while (i < bytes.size())
    {
        if (need == 0 && is_ascii(bytes[i]))
        {
            // a run of ASCII: whole words while there are some, then byte by byte to its end
            while (bytes.size() - i >= word && ascii_word(bytes.data() + i)) i += word;
            while (i < bytes.size() && is_ascii(bytes[i])) ++i;
            continue;
        }
        if (!feed(bytes[i++])) return false;
    }
    return true;
}

}
