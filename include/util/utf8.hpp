#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace net::util
{

// utf8_validator checks that bytes are UTF-8 (RFC 3629: no overlong forms, no surrogates, nothing past U+10FFFF), fed
// in any pieces: a character may be split across feeds (e.g. a WebSocket message's frames). Once a feed has returned
// false, the validator's state is meaningless until reset().
class utf8_validator
{
public:
    // feed checks bytes after what came before: false at the first that can't be where it is.
    [[nodiscard]] bool feed(std::span<const std::byte> bytes) noexcept;

    // feed checks one byte.
    [[nodiscard]] bool feed(std::byte b) noexcept
    {
        const auto v = static_cast<std::uint8_t>(b);
        if (need == 0)
        {
            if (v < 0x80) return true;
            if (v >= 0xc2 && v <= 0xdf) return expect(1, 0x80, 0xbf);
            if (v == 0xe0) return expect(2, 0xa0, 0xbf); // no overlong 3-byte forms
            if ((v >= 0xe1 && v <= 0xec) || v == 0xee || v == 0xef) return expect(2, 0x80, 0xbf);
            if (v == 0xed) return expect(2, 0x80, 0x9f); // no surrogates
            if (v == 0xf0) return expect(3, 0x90, 0xbf); // no overlong 4-byte forms
            if (v >= 0xf1 && v <= 0xf3) return expect(3, 0x80, 0xbf);
            if (v == 0xf4) return expect(3, 0x80, 0x8f); // nothing past U+10FFFF
            return false;
        }
        if (v < low || v > high) return false;
        --need;
        low  = 0x80;
        high = 0xbf;
        return true;
    }

    // complete is whether no character is part way through: whether what was fed so far may end there.
    [[nodiscard]] bool complete() const noexcept { return need == 0; }

    void reset() noexcept { *this = utf8_validator{}; }

private:
    bool expect(std::uint8_t bytes, std::uint8_t lo, std::uint8_t hi) noexcept
    {
        need = bytes;
        low  = lo;
        high = hi;
        return true;
    }

    std::uint8_t need = 0;    // continuation bytes still to come
    std::uint8_t low  = 0x80; // the next continuation byte's range (narrower after some first bytes)
    std::uint8_t high = 0xbf;
};

}
