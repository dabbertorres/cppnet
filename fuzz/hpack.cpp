// fuzz_hpack checks that HPACK's decoder doesn't crash on any header block, and that:
// - a block it decodes, encoded again, decodes to the same fields.
// - a Huffman coding it accepts is the only coding of what it decodes to: encoding that gives the input back.
// - any string's Huffman coding decodes to it.
#include "http/hpack.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

namespace hpack = net::http::hpack;

[[noreturn]] void fail(const char* what, std::span<const std::byte> input)
{
    std::print(stderr, "PROPERTY FAILED: {}\n  input:", what);
    for (const auto b : input) std::print(stderr, " {:02x}", std::to_integer<unsigned>(b));
    std::println(stderr, "");
    std::abort();
}

// owned_field is a decoded field, copied (as a field's views only last during decoding's callback).
struct owned_field
{
    std::string name;
    std::string value;
    bool        never_indexed = false;

    bool operator==(const owned_field&) const = default;
};

bool decode(hpack::decoder& dec, std::span<const std::byte> block, std::vector<owned_field>& out)
{
    return !dec.decode(block,
                       [&](const hpack::field& f)
                       {
                           out.push_back({.name          = std::string{f.name},
                                          .value         = std::string{f.value},
                                          .never_indexed = f.never_indexed});
                       });
}

void check_blocks(std::span<const std::byte> input)
{
    hpack::decoder           dec;
    std::vector<owned_field> fields;
    if (!decode(dec, input, fields)) return;

    std::vector<hpack::field> to_encode;
    to_encode.reserve(fields.size());
    for (const auto& f : fields)
        to_encode.push_back({.name = f.name, .value = f.value, .never_indexed = f.never_indexed});

    hpack::encoder         enc;
    std::vector<std::byte> block;
    enc.encode(to_encode, block);

    hpack::decoder           again;
    std::vector<owned_field> decoded;
    if (!decode(again, block, decoded)) fail("the encoder's block doesn't decode", input);
    if (decoded != fields) fail("the encoder's block decodes to other fields", input);
}

void check_huffman(std::span<const std::byte> input)
{
    std::string decoded;
    if (hpack::huffman_decode(input, decoded))
    {
        std::vector<std::byte> encoded;
        hpack::huffman_encode(decoded, encoded);
        if (!std::ranges::equal(encoded, input)) fail("huffman_decode() accepted a coding encode doesn't make", input);
    }

    const std::string_view text{reinterpret_cast<const char*>(input.data()), input.size()};

    std::vector<std::byte> encoded;
    hpack::huffman_encode(text, encoded);
    if (encoded.size() != hpack::huffman_size(text)) fail("huffman_size() isn't huffman_encode()'s size", input);

    std::string round_trip;
    if (!hpack::huffman_decode(encoded, round_trip) || round_trip != text)
        fail("huffman_encode() doesn't round-trip", input);
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span input{reinterpret_cast<const std::byte*>(data), size};

    check_blocks(input);
    check_huffman(input);
    return 0;
}
