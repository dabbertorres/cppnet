#include "encoding/hex.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace net::encoding::hex
{

std::array<char, 2> encode(std::byte b) noexcept
{
    auto v = static_cast<std::uint8_t>(b);

    auto hi = static_cast<char>((v & 0xF0) >> 4);
    auto lo = static_cast<char>(v & 0x0F);

    return {
        static_cast<char>(hi < 0xA ? (hi + '0') : (hi - 0xA + 'A')),
        static_cast<char>(lo < 0xA ? (lo + '0') : (lo - 0xA + 'A')),
    };
}

std::string encode(std::span<const std::byte> data) noexcept
{
    std::string out(encoded_size(data.size()), 0);

    for (auto i = 0u; i < data.size(); ++i)
    {
        auto chars     = encode(data[i]);
        out[i * 2 + 0] = chars[0];
        out[i * 2 + 1] = chars[1];
    }

    return out;
}

std::byte decode(char hi, char lo) noexcept
{
    constexpr auto ensure_uppercase = [](char c) { return c > 'F' ? static_cast<char>(c - ('a' - 'A')) : c; };

    constexpr auto parse = [](char c) { return static_cast<std::uint8_t>(c >= 'A' ? c - 'A' + 0xA : c - '0'); };

    hi = ensure_uppercase(hi);
    lo = ensure_uppercase(lo);

    auto upper = parse(hi);
    auto lower = parse(lo);

    return static_cast<std::byte>((upper << 4) | lower);
}

std::optional<std::vector<std::byte>> decode(std::span<const char> data) noexcept
{
    if (data.size() % 2 != 0) return std::nullopt;

    constexpr auto validate = [](char c)
    { return ('0' <= c && c <= '9') || ('A' <= c && c <= 'F') || ('a' <= c && c <= 'f'); };

    std::vector<std::byte> out(decoded_size(data.size()));

    for (auto i = 0u; i < out.size(); ++i)
    {
        auto hi = data[i * 2 + 0];
        auto lo = data[i * 2 + 1];

        if (!validate(hi) || !validate(lo)) return std::nullopt;

        out[i] = decode(hi, lo);
    }

    return out;
}

}
