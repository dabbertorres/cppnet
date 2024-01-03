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

void encode_to(std::span<const std::byte> data, std::string& out, std::size_t offset = 0) noexcept;
void encode_to(std::byte data, std::string& out, std::size_t offset = 0) noexcept;

std::byte                             decode(char hi, char lo) noexcept;
std::optional<std::vector<std::byte>> decode(std::span<const char> data) noexcept;

bool decode_to(std::span<const char> data, std::span<std::byte> out) noexcept;

}
