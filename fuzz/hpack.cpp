// fuzz_hpack checks that HPACK's decoder doesn't crash on any header blocks, and that:
// - blocks it decodes, encoded again (by one encoder), decode (by another decoder) to the same fields.
// - a Huffman coding it accepts is the only coding of what it decodes to: encoding that gives the input back.
// - any string's Huffman coding decodes to it.
//
// The input is a connection's header blocks, decoded in order by one decoder: each record is a control byte (whose high
// bit, set, changes the table's size before the block, to its other 7 bits' 64s: a SETTINGS_HEADER_TABLE_SIZE, as
// acknowledged), the block's length (0, or more than there is: the rest), and the block. The input as a whole is also
// a Huffman coding, and a string to code.
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
#include <system_error>
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
    // the connection's decoder, and an encoder and decoder that send what it decoded again (the peer's settings
    // changing for both).
    hpack::decoder dec;
    hpack::encoder enc;
    hpack::decoder again;

    for (auto rest = input; !rest.empty();)
    {
        const auto control = std::to_integer<std::uint8_t>(rest[0]);
        rest               = rest.subspan(1);
        if ((control & 0x80U) != 0)
        {
            const auto size = std::size_t{64} * (control & 0x7fU);
            dec.set_max_table_size(size);
            enc.set_max_table_size(size);
            again.set_max_table_size(size);
        }

        std::size_t length = rest.empty() ? 0 : std::to_integer<std::size_t>(rest[0]);
        rest               = rest.subspan(rest.empty() ? 0 : 1);
        if (length == 0 || length > rest.size()) length = rest.size();
        const auto block = rest.first(length);
        rest             = rest.subspan(length);

        // a block that can't be decoded ends the connection; one whose list is too long is decoded to the end (to keep
        // the table in step), without its fields.
        std::vector<owned_field> fields;
        const auto               err = dec.decode(block,
                                                  [&](const hpack::field& f)
                                                  {
                                        fields.push_back({.name          = std::string{f.name},
                                                          .value         = std::string{f.value},
                                                          .never_indexed = f.never_indexed});
                                                  });
        if (err == std::errc::invalid_argument) return;
        if (err) continue;

        std::vector<hpack::field> to_encode;
        to_encode.reserve(fields.size());
        for (const auto& f : fields)
            to_encode.push_back({.name = f.name, .value = f.value, .never_indexed = f.never_indexed});

        std::vector<std::byte> encoded;
        enc.encode(to_encode, encoded);

        std::vector<owned_field> decoded;
        if (!decode(again, encoded, decoded)) fail("the encoder's block doesn't decode", input);
        if (decoded != fields) fail("the encoder's block decodes to other fields", input);
    }
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
