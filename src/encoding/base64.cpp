#include "encoding/base64.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "util/hash.hpp"

namespace std
{

template<>
struct hash<net::encoding::base64::alphabet>
{
    std::size_t operator()(const net::encoding::base64::alphabet& alphabet) const noexcept
    {
        std::size_t h = std::hash<std::size_t>{}(alphabet.size());
        for (const auto c : alphabet)
        {
            h = net::util::detail::hash_combine(h, c);
        }

        return h;
    }
};

}

namespace
{

using net::encoding::base64::alphabet;
using net::encoding::base64::decode_map;

// NOLINTBEGIN(cert-err58-cpp,cppcoreguidelines-avoid-non-const-global-variables)
std::shared_mutex                        decoders_cache_mu;
std::unordered_map<alphabet, decode_map> decoders_cache;
// NOLINTEND(cert-err58-cpp,cppcoreguidelines-avoid-non-const-global-variables)

decode_map build_decoder(const alphabet& alphabet)
{
    // TODO: benchmark whether caching is actually worthwhile or not
    //       or maybe just thread_local caches?

    {
        std::shared_lock read_lock{decoders_cache_mu};
        if (auto it = decoders_cache.find(alphabet); it != decoders_cache.end()) return it->second;
    }

    std::lock_guard write_lock{decoders_cache_mu};

    // double check the decode_map wasn't created by someone else while we were waiting
    if (auto it = decoders_cache.find(alphabet); it != decoders_cache.end()) return it->second;

    decode_map out;
    out.fill(static_cast<std::byte>(0xff));

    for (std::size_t i = 0; i < alphabet.size(); ++i)
    {
        out[static_cast<std::size_t>(alphabet[i])] = static_cast<std::byte>(i);
    }

    decoders_cache[alphabet] = out;

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
        encode_triplet(&out[out_i], &data.data()[in_i]);
    }

    auto wrote = encode_trailing(&out[out_i], &data.data()[in_i], data.size() - in_i);
    if (padchar != 0)
    {
        for (auto i = out_i + wrote; i < out.size(); ++i)
        {
            out[i] = padchar;
        }
    }

    return out;
}

std::optional<std::vector<std::byte>> encoding::decode(std::span<const char> data) const noexcept
{
    auto out_size = decoded_size(data.size());
    if (padchar != 0)
    {
        auto might_have_padding = data.last(2);
        out_size -= static_cast<std::size_t>(std::count(might_have_padding.begin(), might_have_padding.end(), padchar));
    }

    std::vector<std::byte> out(out_size);

    for (std::size_t in_i = 0, out_i = 0; in_i < data.size(); in_i += 4, out_i += 3)
    {
        auto valid = decode_triplet(&out[out_i], &data.data()[in_i], data.size() - in_i);
        if (!valid) return std::nullopt;
    }

    return out;
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

bool encoding::decode_triplet(std::byte* out, const char* data, std::size_t remaining) const noexcept
{
    std::array<std::byte, 4> buf{};
    std::size_t              buflen = 0;

    std::size_t lookahead = std::min(remaining, 4ul);
    std::size_t i         = 0u;
    for (; i < lookahead; ++i)
    {
        auto in  = data[i];
        auto dec = decoder[static_cast<std::size_t>(in)];
        if (dec == static_cast<std::byte>(0xff))
        {
            // padding/done
            if (padchar != 0)
            {
                while (i < lookahead && in == padchar)
                {
                    ++i;
                    in = data[i];
                }
            }

            break;
        }

        buf[i] = dec;
        ++buflen;
    }

    // trailing garbage or invalid character
    if (i < lookahead) return false;

    // clang-format off
        std::uint32_t combined = (static_cast<std::uint32_t>(buf[0]) << 18)
                               | (static_cast<std::uint32_t>(buf[1]) << 12)
                               | (static_cast<std::uint32_t>(buf[2]) << 6)
                               | (static_cast<std::uint32_t>(buf[3]) << 0);
    // clang-format on

    if (buflen >= 4) out[2] = static_cast<std::byte>(combined >> 0);
    if (buflen >= 3) out[1] = static_cast<std::byte>(combined >> 8);
    if (buflen >= 2) out[0] = static_cast<std::byte>(combined >> 16);

    return true;
}

}
