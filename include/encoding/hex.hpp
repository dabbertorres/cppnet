#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace net::encoding::hex
{

constexpr std::size_t encoded_size(std::size_t input_size) noexcept { return input_size * 2; }
constexpr std::size_t decoded_size(std::size_t input_size) noexcept { return input_size / 2; }

std::array<char, 2> encode(std::byte b) noexcept;
std::string         encode(std::span<const std::byte> data) noexcept;

std::byte                             decode(char hi, char lo) noexcept;
std::optional<std::vector<std::byte>> decode(std::span<const char> data) noexcept;

}
