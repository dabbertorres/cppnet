#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <system_error>

#include "io/reader.hpp"
#include "io/writer.hpp"

namespace net::crypto::aes
{

std::error_condition encrypt(io::reader& input, io::writer& output, const std::array<std::byte, 16>& key) noexcept;
std::error_condition encrypt(io::reader& input, io::writer& output, const std::array<std::byte, 24>& key) noexcept;
std::error_condition encrypt(io::reader& input, io::writer& output, const std::array<std::byte, 32>& key) noexcept;

std::error_condition decrypt(io::reader& input, io::writer& output, const std::array<std::byte, 16>& key) noexcept;
std::error_condition decrypt(io::reader& input, io::writer& output, const std::array<std::byte, 24>& key) noexcept;
std::error_condition decrypt(io::reader& input, io::writer& output, const std::array<std::byte, 32>& key) noexcept;

}
