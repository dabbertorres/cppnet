// fuzz_hpack checks that HPACK's decoder doesn't crash on any header blocks, and that:
// - blocks it decodes, encoded again (by one encoder indexing, hpack::indexing::automatic, and by one that doesn't),
//   decode (by another decoder each) to the same fields, but that the indexing one sends what sensitive() names as
//   never indexed.
// - after each block, the indexing encoder's dynamic table holds the same fields as its decoder's, in order (they're
//   in step), none of them sensitive() ones; and a field it was given as never indexed is in it only if it was before
//   the block, or the block also has it not marked so (whose first, if it was new, was indexed).
// - a Huffman coding it accepts is the only coding of what it decodes to: encoding that gives the input back.
// - any string's Huffman coding decodes to it.
//
// The input is a connection's header blocks, decoded in order by one decoder: each record is a control byte (whose high
// bit, set, changes the table's size before the block, to its low 6 bits' 128s: a SETTINGS_HEADER_TABLE_SIZE, as
// acknowledged; and with the next bit set too, changes it again to 4096, so it shrinks then grows, or grows then
// shrinks, between blocks), the block's length (0, or more than there is: the rest), and the block. The input as a
// whole is also a Huffman coding, and a string to code.
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

// table_of is a dynamic table's fields, newest first.
std::vector<owned_field> table_of(const hpack::dynamic_table& table)
{
    std::vector<owned_field> out;
    for (std::size_t i = 0; i < table.count(); ++i)
    {
        const auto f = table.at(i);
        out.push_back({.name = std::string{f.name}, .value = std::string{f.value}});
    }
    return out;
}

// peer is an encoder and the decoder it sends to.
struct peer
{
    explicit peer(hpack::indexing mode)
        : enc{mode}
    {}

    hpack::encoder enc;
    hpack::decoder dec;

    void set_max_table_size(std::size_t size)
    {
        enc.set_max_table_size(size);
        dec.set_max_table_size(size);
    }

    // send encodes fields and decodes them again, failing the property if they don't decode to the fields (never
    // indexed as want_never says).
    void
    send(std::span<const hpack::field> fields, const std::vector<bool>& want_never, std::span<const std::byte> input)
    {
        std::vector<std::byte> encoded;
        enc.encode(fields, encoded);

        std::vector<owned_field> decoded;
        if (!decode(dec, encoded, decoded)) fail("the encoder's block doesn't decode", input);
        if (decoded.size() != fields.size()) fail("the encoder's block decodes to other fields", input);
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            const auto& f = fields[i];
            if (decoded[i].name != f.name || decoded[i].value != f.value || decoded[i].never_indexed != want_never[i])
                fail("the encoder's block decodes to other fields", input);
        }
    }
};

// check_tables checks the indexing encoder's table after a block of fields: in step with its decoder's, and with
// nothing that wasn't to be indexed. before is what it held before the block.
void check_tables(const peer&                     indexing,
                  const std::vector<owned_field>& before,
                  const std::vector<owned_field>& fields,
                  std::span<const std::byte>      input)
{
    const auto after = table_of(indexing.enc.table());
    if (after != table_of(indexing.dec.table())) fail("the encoder's table isn't its decoder's", input);

    for (const auto& entry : after)
    {
        if (hpack::sensitive(entry.name)) fail("a sensitive field is in the encoder's table", input);
        if (std::ranges::contains(before, entry)) continue;

        // new in this block: so the block has it not marked never indexed.
        const auto unmarked = [&](const owned_field& f)
        { return !f.never_indexed && f.name == entry.name && f.value == entry.value; };
        if (!std::ranges::any_of(fields, unmarked)) fail("a never-indexed field is in the encoder's table", input);
    }
}

// table_sizes is what a record's control byte changes the table's size to, in turn: none, one, or one and then 4096.
std::vector<std::size_t> table_sizes(std::uint8_t control)
{
    if ((control & 0x80U) == 0) return {};

    std::vector<std::size_t> sizes{std::size_t{128} * (control & 0x3fU)};
    if ((control & 0x40U) != 0) sizes.push_back(hpack::default_table_size);
    return sizes;
}

void check_blocks(std::span<const std::byte> input)
{
    // the connection's decoder, and encoders (with their decoders) that send what it decoded again (the peer's settings
    // changing for all).
    hpack::decoder dec;
    peer           indexing{hpack::indexing::automatic};
    peer           plain{hpack::indexing::none};

    for (auto rest = input; !rest.empty();)
    {
        const auto control = std::to_integer<std::uint8_t>(rest[0]);
        rest               = rest.subspan(1);
        for (const auto size : table_sizes(control))
        {
            dec.set_max_table_size(size);
            indexing.set_max_table_size(size);
            plain.set_max_table_size(size);
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
        std::vector<bool>         never;
        std::vector<bool>         never_or_sensitive;
        to_encode.reserve(fields.size());
        for (const auto& f : fields)
        {
            to_encode.push_back({.name = f.name, .value = f.value, .never_indexed = f.never_indexed});
            never.push_back(f.never_indexed);
            never_or_sensitive.push_back(f.never_indexed || hpack::sensitive(f.name));
        }

        const auto before = table_of(indexing.enc.table());
        indexing.send(to_encode, never_or_sensitive, input);
        plain.send(to_encode, never, input);

        check_tables(indexing, before, fields, input);
        if (plain.enc.table().count() != 0) fail("the encoder without indexing added to its table", input);
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
