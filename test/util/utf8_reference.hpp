#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace test
{

// utf8_result is what utf8_reference finds: the first byte that no UTF-8 can have where it is (bytes' size if none),
// and whether the bytes end on a character's end.
struct utf8_result
{
    std::size_t bad      = 0;
    bool        complete = false;

    bool operator==(const utf8_result&) const = default;
};

// utf8_reference checks bytes as RFC 3629 defines UTF-8, by code points rather than byte ranges (as
// util::utf8_validator does), to test it against: a byte fails as soon as the bytes of its character so far can't be
// completed to a code point that's in range for its length (no overlong forms), isn't a surrogate and isn't past
// U+10FFFF.
inline utf8_result utf8_reference(std::span<const std::byte> bytes) noexcept
{
    // completable is whether a character of length bytes, whose first known ones gave the code point's top bits cp,
    // can still be one.
    const auto completable = [](std::uint32_t cp, std::size_t known, std::size_t length)
    {
        constexpr std::array<std::uint32_t, 5> mins{0, 0, 0x80, 0x800, 0x1'0000};
        const auto                             rest = 6 * (length - known);
        const std::uint32_t                    lo   = std::max(cp << rest, mins[length]);
        const std::uint32_t hi = std::min((cp << rest) | ((std::uint32_t{1} << rest) - 1), std::uint32_t{0x10'ffff});
        if (lo > hi) return false;
        return lo < 0xd800 || hi > 0xdfff; // not all surrogates
    };

    std::size_t i = 0;
    while (i < bytes.size())
    {
        const auto lead = static_cast<std::uint8_t>(bytes[i]);
        if (lead < 0x80)
        {
            ++i;
            continue;
        }

        std::size_t length = 0;
        if ((lead & 0xe0) == 0xc0) length = 2;
        else if ((lead & 0xf0) == 0xe0) length = 3;
        else if ((lead & 0xf8) == 0xf0) length = 4;
        else return {.bad = i}; // a continuation byte, or a 5- or 6-byte form's lead

        std::uint32_t cp = lead & (0x7fU >> length);
        if (!completable(cp, 1, length)) return {.bad = i};
        for (std::size_t k = 1; k < length; ++k)
        {
            if (i + k == bytes.size()) return {.bad = bytes.size(), .complete = false};

            const auto next = static_cast<std::uint8_t>(bytes[i + k]);
            if ((next & 0xc0) != 0x80) return {.bad = i + k};
            cp = (cp << 6) | (next & 0x3fU);
            if (!completable(cp, k + 1, length)) return {.bad = i + k};
        }
        i += length;
    }
    return {.bad = bytes.size(), .complete = true};
}

}
