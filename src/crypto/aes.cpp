#include "crypto/aes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

consteval std::size_t num_rounds(std::size_t key_length_bits) noexcept { return key_length_bits / 32 + 6; }
consteval std::size_t buffer_size(std::size_t key_length_bits) noexcept { return key_length_bits / 8 + 28; }

consteval std::uint32_t substitution_box_forward(std::uint32_t value) noexcept {return value ^ }

std::array<std::byte, 16> key_schedule() noexcept
{}

template<std::size_t key_bits>
struct state
{
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    const std::array<std::byte, key_bits>& master_key;

    std::array<std::uint32_t, buffer_size(key_bits)> buffer;
};

namespace net::crypto::aes
{

std::error_condition encrypt(io::reader& input, io::writer& output, const std::array<std::byte, 16>& key) noexcept {}

std::error_condition encrypt(io::reader& input, io::writer& output, const std::array<std::byte, 24>& key) noexcept {}

std::error_condition encrypt(io::reader& input, io::writer& output, const std::array<std::byte, 32>& key) noexcept {}

std::error_condition decrypt(io::reader& input, io::writer& output, const std::array<std::byte, 16>& key) noexcept {}

std::error_condition decrypt(io::reader& input, io::writer& output, const std::array<std::byte, 24>& key) noexcept {}

std::error_condition decrypt(io::reader& input, io::writer& output, const std::array<std::byte, 32>& key) noexcept {}

}
