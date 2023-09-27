#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
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
    return (input_size * 8 + 5) / 6;
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
    encoding(alphabet alphabet = standard_alphabet(), char padchar = '=') noexcept;

    [[nodiscard]] std::size_t encoded_size(std::size_t input_size) const noexcept;
    [[nodiscard]] std::size_t decoded_size(std::size_t input_size) const noexcept;

    [[nodiscard]] std::string encode(std::span<const std::byte> data) const noexcept;

    // returns an empty optional if the input data is invalid/corrupt.
    [[nodiscard]] std::optional<std::vector<std::byte>> decode(std::span<const char> data) const noexcept;

private:
    void encode_triplet(char* out, const std::byte* data) const noexcept;

    // returns the number of characters written to out.
    std::size_t encode_trailing(char* out, const std::byte* data, std::size_t remaining) const noexcept;

    // returns false if the input data is invalid/corrupt.
    bool decode_triplet(std::byte* out, const char* data, std::size_t remaining) const noexcept;

    alphabet   encoder;
    decode_map decoder;
    char       padchar;
};

}
