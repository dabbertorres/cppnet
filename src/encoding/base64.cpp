#include "encoding/base64.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{

using net::encoding::base64::alphabet;
using net::encoding::base64::decode_map;

decode_map build_decoder(const alphabet& alphabet) noexcept
{
    decode_map out;
    out.fill(static_cast<std::byte>(0xff));

    for (std::size_t i = 0; i < alphabet.size(); ++i)
    {
        // NOTE: through unsigned char, as a char may be negative.
        out[static_cast<unsigned char>(alphabet[i])] = static_cast<std::byte>(i);
    }

    return out;
}

}

namespace net::encoding::base64
{

encoding::encoding(alphabet alphabet, char padchar) noexcept
    : encoder{alphabet}
    , decoder{build_decoder(alphabet)}
    , padchar{padchar}
{}

std::size_t encoding::encoded_size(std::size_t input_size) const noexcept
{
    return ::net::encoding::base64::encoded_size(input_size, padchar != 0);
}

std::size_t encoding::decoded_size(std::size_t input_size) const noexcept
{
    return ::net::encoding::base64::decoded_size(input_size, padchar != 0);
}

std::string encoding::encode(std::span<const std::byte> data) const noexcept
{
    std::string out(encoded_size(data.size()), padchar);

    std::size_t out_i  = 0;
    std::size_t in_i   = 0;
    std::size_t blocks = data.size() / 3 * 3; // round down to a multiple of 3
    for (; in_i < blocks; in_i += 3, out_i += 4)
    {
        encode_triplet(&out[out_i], &data[in_i]);
    }

    // NOTE: data.data() + in_i, not &data[in_i], which is out of range when there's nothing left.
    auto wrote = encode_trailing(&out[out_i], data.data() + in_i, data.size() - in_i);
    if (padchar != 0)
    {
        for (auto i = out_i + wrote; i < out.size(); ++i)
        {
            out[i] = padchar;
        }
    }

    return out;
}

std::expected<std::vector<std::byte>, std::error_condition> encoding::decode(std::string_view data) const
{
    std::vector<std::byte> out(decoded_size(data.size()));

    auto wrote = decode_to(data, out);
    if (!wrote) return std::unexpected{wrote.error()};

    out.resize(*wrote);
    return out;
}

std::expected<std::size_t, std::error_condition> encoding::decode_to(std::string_view     data,
                                                                     std::span<std::byte> out) const noexcept
{
    const auto invalid = std::unexpected{std::make_error_condition(std::errc::invalid_argument)};

    if (padchar != 0)
    {
        if (data.size() % 4 != 0) return invalid;

        // at most two: a third would be where a character has to be (and fails to decode, below).
        if (data.ends_with(padchar)) data.remove_suffix(1);
        if (data.ends_with(padchar)) data.remove_suffix(1);
    }

    // the characters after the last whole group of 4: one can't be (it's only 6 bits of a byte).
    const auto tail = data.size() % 4;
    if (tail == 1) return invalid;

    const auto groups = data.size() / 4;
    const auto size   = (groups * 3) + (tail == 0 ? 0 : tail - 1);
    if (out.size() < size) return std::unexpected{std::make_error_condition(std::errc::value_too_large)};

    for (std::size_t i = 0; i < groups; ++i)
    {
        const auto a = value(data[(i * 4) + 0]);
        const auto b = value(data[(i * 4) + 1]);
        const auto c = value(data[(i * 4) + 2]);
        const auto d = value(data[(i * 4) + 3]);

        // NOTE: values are at most 0x3f, so any of the top two bits means one wasn't in the alphabet.
        if (((a | b | c | d) & 0xc0) != 0) return invalid;

        const std::uint32_t combined =
            (std::uint32_t{a} << 18) | (std::uint32_t{b} << 12) | (std::uint32_t{c} << 6) | d;

        out[(i * 3) + 0] = static_cast<std::byte>(combined >> 16);
        out[(i * 3) + 1] = static_cast<std::byte>(combined >> 8);
        out[(i * 3) + 2] = static_cast<std::byte>(combined);
    }

    if (tail != 0)
    {
        const auto a = value(data[(groups * 4) + 0]);
        const auto b = value(data[(groups * 4) + 1]);
        const auto c = tail == 3 ? value(data[(groups * 4) + 2]) : std::uint8_t{0};
        if (((a | b | c) & 0xc0) != 0) return invalid;

        const std::uint32_t combined = (std::uint32_t{a} << 18) | (std::uint32_t{b} << 12) | (std::uint32_t{c} << 6);

        // the bits after the last whole byte must be zero: 4 of them after 1 byte, 2 after 2.
        const std::uint32_t unused = tail == 2 ? 0x00'ffff : 0x00'00ff;
        if ((combined & unused) != 0) return invalid;

        out[(groups * 3) + 0] = static_cast<std::byte>(combined >> 16);
        if (tail == 3) out[(groups * 3) + 1] = static_cast<std::byte>(combined >> 8);
    }

    return size;
}

void encoding::encode_triplet(char* out, const std::byte* data) const noexcept
{
    // clang-format off
        std::uint32_t combined = (static_cast<std::uint32_t>(data[0]) << 16)
                               | (static_cast<std::uint32_t>(data[1]) << 8)
                               | (static_cast<std::uint32_t>(data[2]) << 0);
    // clang-format on

    out[0] = encoder[(combined >> 18) & 0b0011'1111];
    out[1] = encoder[(combined >> 12) & 0b0011'1111];
    out[2] = encoder[(combined >> 6) & 0b0011'1111];
    out[3] = encoder[(combined >> 0) & 0b0011'1111];
}

std::size_t encoding::encode_trailing(char* out, const std::byte* data, std::size_t remaining) const noexcept
{
    // remaining will only ever be 0, 1, or 2
    switch (remaining)
    {
    case 1:
    {
        std::uint32_t combined = static_cast<std::uint32_t>(data[0]) << 16;
        out[0]                 = encoder[(combined >> 18) & 0b0011'1111];
        out[1]                 = encoder[(combined >> 12) & 0b0011'1111];
        return 2;
    }

    case 2:
    {
        std::uint32_t combined = static_cast<std::uint32_t>(data[0]) << 16 | static_cast<std::uint32_t>(data[1]) << 8;

        out[0] = encoder[(combined >> 18) & 0b0011'1111];
        out[1] = encoder[(combined >> 12) & 0b0011'1111];
        out[2] = encoder[(combined >> 6) & 0b0011'1111];
        return 3;
    }

    default: return 0;
    }
}

std::uint8_t encoding::value(char c) const noexcept
{
    // NOTE: through unsigned char, as a char may be negative.
    return static_cast<std::uint8_t>(decoder[static_cast<unsigned char>(c)]);
}

}
