#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace net::encoding::base64
{

using alphabet   = std::array<char, 64>;
using decode_map = std::array<std::byte, 256>;

consteval alphabet standard_alphabet() noexcept
{
    return std::to_array<char>({
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
        'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
        's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/',
    });
}

consteval alphabet url_alphabet() noexcept
{
    return std::to_array<char>({
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
        'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
        's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '-', '_',
    });
}

constexpr std::size_t encoded_size(std::size_t input_size, bool padding = true) noexcept
{
    if (padding) return (input_size + 2) / 3 * 4;
    return ((input_size * 8) + 5) / 6;
}

constexpr std::size_t decoded_size(std::size_t input_size, bool padding = true) noexcept
{
    if (padding) return input_size / 4 * 3;
    return input_size * 6 / 8;
}

struct encoding
{
public:
    // to disable padding, pass 0 for padchar.
    explicit encoding(alphabet alphabet = standard_alphabet(), char padchar = '=') noexcept;

    [[nodiscard]] std::size_t encoded_size(std::size_t input_size) const noexcept;

    // decoded_size is the most input_size characters decode to (fewer if they end with padding).
    [[nodiscard]] std::size_t decoded_size(std::size_t input_size) const noexcept;

    [[nodiscard]] std::string encode(std::span<const std::byte> data) const noexcept;

    // decode decodes data, which must be as encode makes it, failing with invalid_argument if it isn't (RFC 4648):
    // - only characters of the alphabet, so no whitespace.
    // - with padding (if the encoding has a padchar): a multiple of 4 characters, with at most two padchars, at the
    //   end. Without: no padchars, and not one more than a multiple of 4.
    // - the bits of the last character that aren't part of a byte are zero, so that data is the only encoding of
    //   what it decodes to.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::error_code> decode(std::string_view data) const;

    // decode_to decodes data (as decode does) into out, returning how many bytes it wrote. It fails with
    // value_too_large if they don't fit: decoded_size(data.size()) always do.
    [[nodiscard]] std::expected<std::size_t, std::error_code> decode_to(std::string_view     data,
                                                                        std::span<std::byte> out) const noexcept;

private:
    void encode_triplet(char* out, const std::byte* data) const noexcept;

    // returns the number of characters written to out.
    std::size_t encode_trailing(char* out, const std::byte* data, std::size_t remaining) const noexcept;

    // value is the 6 bits c encodes, or 0xff if it isn't in the alphabet.
    [[nodiscard]] std::uint8_t value(char c) const noexcept;

    alphabet   encoder;
    decode_map decoder;
    char       padchar;
};

}
