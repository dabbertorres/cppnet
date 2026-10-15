#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string_view>

namespace net::util
{

struct uuid
{
    enum class variant_type : std::uint8_t
    {
        rfc4122,
        microsoft,
        future,
        reserved,
    };

    static uuid generate_v4();
    static uuid generate_v7();

    static uuid nil();
    static uuid max();

    [[nodiscard]] std::uint8_t version() const noexcept { return (static_cast<std::uint8_t>(data[6]) & 0xf0) >> 4; }
    [[nodiscard]] variant_type variant() const noexcept
    {
        using enum variant_type;

        if ((data[8] & std::byte(0xc0)) == std::byte(0x80)) return rfc4122;

        if ((data[8] & std::byte(0xe0)) == std::byte(0xc0)) return microsoft;

        if ((data[8] & std::byte(0xe0)) == std::byte(0xe)) return future;

        return reserved;
    }

    std::array<std::byte, 16> data;

    std::strong_ordering operator<=>(const uuid&) const = default;

private:
    void set_version(std::uint8_t version) noexcept { data[6] = std::byte(version << 4) | (data[6] & std::byte(0x0f)); }
    void set_variant(std::uint8_t variant) noexcept { data[8] = std::byte(variant << 4) | (data[8] & std::byte(0x3f)); }
};

uuid uuid_parse(std::string_view sv);

}

template<>
struct std::formatter<net::util::uuid, char>
{
    template<typename ParseContext>
    constexpr ParseContext::iterator parse(ParseContext& ctx)
    {
        auto it = ctx.begin();
        if (it == ctx.end()) return it;

        switch (*it)
        {
        case 'u':
        case 'U':
            upper = true;
            ++it;
            break;

        case 'l':
        case 'L':
            upper = false;
            ++it;
            break;

        default: throw std::format_error{"invalid format args for uuid"};
        }

        if (it != ctx.end()) throw std::format_error{"invalid format args for uuid"};

        return it;
    }

    template<typename FmtContext>
    FmtContext::iterator format(const net::util::uuid& value, FmtContext& ctx) const
    {
        const auto& alphabet = upper ? upper_chars : lower_chars;

        auto out = std::ranges::begin(ctx.out());

        out    = write_byte(out, value.data[0], alphabet);
        out    = write_byte(out, value.data[1], alphabet);
        out    = write_byte(out, value.data[2], alphabet);
        out    = write_byte(out, value.data[3], alphabet);
        *out++ = '-';

        out    = write_byte(out, value.data[4], alphabet);
        out    = write_byte(out, value.data[5], alphabet);
        *out++ = '-';

        out    = write_byte(out, value.data[6], alphabet);
        out    = write_byte(out, value.data[7], alphabet);
        *out++ = '-';

        out    = write_byte(out, value.data[8], alphabet);
        out    = write_byte(out, value.data[9], alphabet);
        *out++ = '-';

        out = write_byte(out, value.data[10], alphabet);
        out = write_byte(out, value.data[11], alphabet);
        out = write_byte(out, value.data[12], alphabet);
        out = write_byte(out, value.data[13], alphabet);
        out = write_byte(out, value.data[14], alphabet);
        out = write_byte(out, value.data[15], alphabet);

        return out;
    }

    bool upper = false;

private:
    template<typename FmtContext>
    FmtContext::iterator write_byte(FmtContext::iterator out, std::byte b, const std::array<char, 16>& alphabet) const
    {
        auto v = static_cast<std::uint8_t>(b);
        *out++ = alphabet[(v & 0xf0) >> 4];
        *out++ = alphabet[(v & 0x0f) >> 0];
        return out;
    }

    static constexpr std::array<char, 16> upper_chars{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    static constexpr std::array<char, 16> lower_chars{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
};
