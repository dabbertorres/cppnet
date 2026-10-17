#include "http/hpack.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

using net::http::hpack::field;

// huffman_code is a symbol's code (aligned to the least significant bit), and its length in bits.
using huffman_code = std::pair<std::uint32_t, std::uint8_t>;

// Generated from RFC 7541's text by tools/hpack/gen_tables.py, which checks the code as it parses it.
// RFC 7541, Appendix A: the static table (1-based in the RFC, so index i is entry i - 1).
constexpr std::array<std::pair<std::string_view, std::string_view>, 61> static_table{
    {
     {":authority", ""},
     {":method", "GET"},
     {":method", "POST"},
     {":path", "/"},
     {":path", "/index.html"},
     {":scheme", "http"},
     {":scheme", "https"},
     {":status", "200"},
     {":status", "204"},
     {":status", "206"},
     {":status", "304"},
     {":status", "400"},
     {":status", "404"},
     {":status", "500"},
     {"accept-charset", ""},
     {"accept-encoding", "gzip, deflate"},
     {"accept-language", ""},
     {"accept-ranges", ""},
     {"accept", ""},
     {"access-control-allow-origin", ""},
     {"age", ""},
     {"allow", ""},
     {"authorization", ""},
     {"cache-control", ""},
     {"content-disposition", ""},
     {"content-encoding", ""},
     {"content-language", ""},
     {"content-length", ""},
     {"content-location", ""},
     {"content-range", ""},
     {"content-type", ""},
     {"cookie", ""},
     {"date", ""},
     {"etag", ""},
     {"expect", ""},
     {"expires", ""},
     {"from", ""},
     {"host", ""},
     {"if-match", ""},
     {"if-modified-since", ""},
     {"if-none-match", ""},
     {"if-range", ""},
     {"if-unmodified-since", ""},
     {"last-modified", ""},
     {"link", ""},
     {"location", ""},
     {"max-forwards", ""},
     {"proxy-authenticate", ""},
     {"proxy-authorization", ""},
     {"range", ""},
     {"referer", ""},
     {"refresh", ""},
     {"retry-after", ""},
     {"server", ""},
     {"set-cookie", ""},
     {"strict-transport-security", ""},
     {"transfer-encoding", ""},
     {"user-agent", ""},
     {"vary", ""},
     {"via", ""},
     {"www-authenticate", ""},
     }
};

// RFC 7541, Appendix B: each symbol's code and its length in bits; 256 is EOS.
constexpr std::array<huffman_code, 257> huffman_codes{
    {
     {0x1ff8, 13},     {0x7f'ffd8, 23},   {0xfff'ffe2, 28}, {0xfff'ffe3, 28}, {0xfff'ffe4, 28},  {0xfff'ffe5, 28},
     {0xfff'ffe6, 28}, {0xfff'ffe7, 28},  {0xfff'ffe8, 28}, {0xff'ffea, 24},  {0x3fff'fffc, 30}, {0xfff'ffe9, 28},
     {0xfff'ffea, 28}, {0x3fff'fffd, 30}, {0xfff'ffeb, 28}, {0xfff'ffec, 28}, {0xfff'ffed, 28},  {0xfff'ffee, 28},
     {0xfff'ffef, 28}, {0xfff'fff0, 28},  {0xfff'fff1, 28}, {0xfff'fff2, 28}, {0x3fff'fffe, 30}, {0xfff'fff3, 28},
     {0xfff'fff4, 28}, {0xfff'fff5, 28},  {0xfff'fff6, 28}, {0xfff'fff7, 28}, {0xfff'fff8, 28},  {0xfff'fff9, 28},
     {0xfff'fffa, 28}, {0xfff'fffb, 28},  {0x14, 6},        {0x3f8, 10},      {0x3f9, 10},       {0xffa, 12},
     {0x1ff9, 13},     {0x15, 6},         {0xf8, 8},        {0x7fa, 11},      {0x3fa, 10},       {0x3fb, 10},
     {0xf9, 8},        {0x7fb, 11},       {0xfa, 8},        {0x16, 6},        {0x17, 6},         {0x18, 6},
     {0x0, 5},         {0x1, 5},          {0x2, 5},         {0x19, 6},        {0x1a, 6},         {0x1b, 6},
     {0x1c, 6},        {0x1d, 6},         {0x1e, 6},        {0x1f, 6},        {0x5c, 7},         {0xfb, 8},
     {0x7ffc, 15},     {0x20, 6},         {0xffb, 12},      {0x3fc, 10},      {0x1ffa, 13},      {0x21, 6},
     {0x5d, 7},        {0x5e, 7},         {0x5f, 7},        {0x60, 7},        {0x61, 7},         {0x62, 7},
     {0x63, 7},        {0x64, 7},         {0x65, 7},        {0x66, 7},        {0x67, 7},         {0x68, 7},
     {0x69, 7},        {0x6a, 7},         {0x6b, 7},        {0x6c, 7},        {0x6d, 7},         {0x6e, 7},
     {0x6f, 7},        {0x70, 7},         {0x71, 7},        {0x72, 7},        {0xfc, 8},         {0x73, 7},
     {0xfd, 8},        {0x1ffb, 13},      {0x7'fff0, 19},   {0x1ffc, 13},     {0x3ffc, 14},      {0x22, 6},
     {0x7ffd, 15},     {0x3, 5},          {0x23, 6},        {0x4, 5},         {0x24, 6},         {0x5, 5},
     {0x25, 6},        {0x26, 6},         {0x27, 6},        {0x6, 5},         {0x74, 7},         {0x75, 7},
     {0x28, 6},        {0x29, 6},         {0x2a, 6},        {0x7, 5},         {0x2b, 6},         {0x76, 7},
     {0x2c, 6},        {0x8, 5},          {0x9, 5},         {0x2d, 6},        {0x77, 7},         {0x78, 7},
     {0x79, 7},        {0x7a, 7},         {0x7b, 7},        {0x7ffe, 15},     {0x7fc, 11},       {0x3ffd, 14},
     {0x1ffd, 13},     {0xfff'fffc, 28},  {0xf'ffe6, 20},   {0x3f'ffd2, 22},  {0xf'ffe7, 20},    {0xf'ffe8, 20},
     {0x3f'ffd3, 22},  {0x3f'ffd4, 22},   {0x3f'ffd5, 22},  {0x7f'ffd9, 23},  {0x3f'ffd6, 22},   {0x7f'ffda, 23},
     {0x7f'ffdb, 23},  {0x7f'ffdc, 23},   {0x7f'ffdd, 23},  {0x7f'ffde, 23},  {0xff'ffeb, 24},   {0x7f'ffdf, 23},
     {0xff'ffec, 24},  {0xff'ffed, 24},   {0x3f'ffd7, 22},  {0x7f'ffe0, 23},  {0xff'ffee, 24},   {0x7f'ffe1, 23},
     {0x7f'ffe2, 23},  {0x7f'ffe3, 23},   {0x7f'ffe4, 23},  {0x1f'ffdc, 21},  {0x3f'ffd8, 22},   {0x7f'ffe5, 23},
     {0x3f'ffd9, 22},  {0x7f'ffe6, 23},   {0x7f'ffe7, 23},  {0xff'ffef, 24},  {0x3f'ffda, 22},   {0x1f'ffdd, 21},
     {0xf'ffe9, 20},   {0x3f'ffdb, 22},   {0x3f'ffdc, 22},  {0x7f'ffe8, 23},  {0x7f'ffe9, 23},   {0x1f'ffde, 21},
     {0x7f'ffea, 23},  {0x3f'ffdd, 22},   {0x3f'ffde, 22},  {0xff'fff0, 24},  {0x1f'ffdf, 21},   {0x3f'ffdf, 22},
     {0x7f'ffeb, 23},  {0x7f'ffec, 23},   {0x1f'ffe0, 21},  {0x1f'ffe1, 21},  {0x3f'ffe0, 22},   {0x1f'ffe2, 21},
     {0x7f'ffed, 23},  {0x3f'ffe1, 22},   {0x7f'ffee, 23},  {0x7f'ffef, 23},  {0xf'ffea, 20},    {0x3f'ffe2, 22},
     {0x3f'ffe3, 22},  {0x3f'ffe4, 22},   {0x7f'fff0, 23},  {0x3f'ffe5, 22},  {0x3f'ffe6, 22},   {0x7f'fff1, 23},
     {0x3ff'ffe0, 26}, {0x3ff'ffe1, 26},  {0xf'ffeb, 20},   {0x7'fff1, 19},   {0x3f'ffe7, 22},   {0x7f'fff2, 23},
     {0x3f'ffe8, 22},  {0x1ff'ffec, 25},  {0x3ff'ffe2, 26}, {0x3ff'ffe3, 26}, {0x3ff'ffe4, 26},  {0x7ff'ffde, 27},
     {0x7ff'ffdf, 27}, {0x3ff'ffe5, 26},  {0xff'fff1, 24},  {0x1ff'ffed, 25}, {0x7'fff2, 19},    {0x1f'ffe3, 21},
     {0x3ff'ffe6, 26}, {0x7ff'ffe0, 27},  {0x7ff'ffe1, 27}, {0x3ff'ffe7, 26}, {0x7ff'ffe2, 27},  {0xff'fff2, 24},
     {0x1f'ffe4, 21},  {0x1f'ffe5, 21},   {0x3ff'ffe8, 26}, {0x3ff'ffe9, 26}, {0xfff'fffd, 28},  {0x7ff'ffe3, 27},
     {0x7ff'ffe4, 27}, {0x7ff'ffe5, 27},  {0xf'ffec, 20},   {0xff'fff3, 24},  {0xf'ffed, 20},    {0x1f'ffe6, 21},
     {0x3f'ffe9, 22},  {0x1f'ffe7, 21},   {0x1f'ffe8, 21},  {0x7f'fff3, 23},  {0x3f'ffea, 22},   {0x3f'ffeb, 22},
     {0x1ff'ffee, 25}, {0x1ff'ffef, 25},  {0xff'fff4, 24},  {0xff'fff5, 24},  {0x3ff'ffea, 26},  {0x7f'fff4, 23},
     {0x3ff'ffeb, 26}, {0x7ff'ffe6, 27},  {0x3ff'ffec, 26}, {0x3ff'ffed, 26}, {0x7ff'ffe7, 27},  {0x7ff'ffe8, 27},
     {0x7ff'ffe9, 27}, {0x7ff'ffea, 27},  {0x7ff'ffeb, 27}, {0xfff'fffe, 28}, {0x7ff'ffec, 27},  {0x7ff'ffed, 27},
     {0x7ff'ffee, 27}, {0x7ff'ffef, 27},  {0x7ff'fff0, 27}, {0x3ff'ffee, 26}, {0x3fff'ffff, 30},
     }
};

constexpr std::uint16_t eos = 256;

std::uint8_t octet(std::byte b) noexcept { return std::to_integer<std::uint8_t>(b); }

// huffman_tree is the Huffman code's tree. It has 256 internal nodes (as the code has 257 symbols, and is complete),
// the root being 0, which are the Huffman decoder's states.
struct huffman_tree
{
    // a child of an internal node that's a leaf: leaf | its symbol.
    static constexpr std::uint16_t leaf = 0x8000;

    // each node's children: another internal node's number, or a leaf. 0 is none, as the root is no one's child.
    std::array<std::array<std::uint16_t, 2>, 256> children{};
    std::array<std::uint8_t, 256>                 depth{};
    std::array<bool, 256>                         all_ones{}; // whether a node's path from the root is all ones
    std::size_t                                   nodes = 1;
};

consteval huffman_tree make_huffman_tree()
{
    huffman_tree tree;
    tree.all_ones[0] = true;

    for (std::size_t symbol = 0; symbol < huffman_codes.size(); ++symbol)
    {
        const auto [code, bits] = huffman_codes[symbol];

        std::size_t node = 0;
        for (int i = bits - 1; i > 0; --i)
        {
            const auto bit   = (code >> i) & 1U;
            auto&      child = tree.children[node][bit];
            if (child == 0)
            {
                // NOTE: an incomplete code would have too many nodes, and indexing past the arrays isn't a constant
                //       expression: a compile error, as the static_assert below is for too few.
                child                     = static_cast<std::uint16_t>(tree.nodes);
                tree.depth[tree.nodes]    = static_cast<std::uint8_t>(tree.depth[node] + 1);
                tree.all_ones[tree.nodes] = tree.all_ones[node] && bit == 1;
                ++tree.nodes;
            }
            node = child;
        }

        tree.children[node][code & 1U] = static_cast<std::uint16_t>(huffman_tree::leaf | symbol);
    }

    return tree;
}

constexpr huffman_tree huffman_code_tree = make_huffman_tree();
static_assert(huffman_code_tree.nodes == 256, "the Huffman code isn't complete");

// The flags of a huffman_step.
constexpr std::uint8_t emits = 1;
// the next state could end the input: its path from the root is all ones, and at most 7 bits (EOS's leading bits, as
// padding).
constexpr std::uint8_t accepting = 2;
// the nibble completes EOS, which mustn't be in a string.
constexpr std::uint8_t fails = 4;

// huffman_step is what decoding a nibble does in a state of the Huffman decoder (a node of the code's tree).
struct huffman_step
{
    std::uint8_t next;   // the state after the nibble
    std::uint8_t flags;  // emits, accepting, fails
    std::uint8_t symbol; // the one decoded, if flags has emits (at most one: the shortest code is 5 bits)
};

// walk_nibble walks the nibble's 4 bits down the code's tree from state, returning to the root after a symbol.
consteval huffman_step walk_nibble(std::size_t state, unsigned nibble)
{
    const auto& tree = huffman_code_tree;

    huffman_step step{};
    std::size_t  node = state;
    for (int i = 3; i >= 0; --i)
    {
        const auto child = tree.children[node][(nibble >> i) & 1U];
        if ((child & huffman_tree::leaf) == 0)
        {
            node = child;
            continue;
        }

        const auto symbol = static_cast<std::uint16_t>(child & ~huffman_tree::leaf);
        if (symbol == eos)
        {
            step.flags |= fails;
            break;
        }

        step.flags |= emits;
        step.symbol = static_cast<std::uint8_t>(symbol);
        node        = 0;
    }

    step.next = static_cast<std::uint8_t>(node);
    if (tree.all_ones[node] && tree.depth[node] <= 7) step.flags |= accepting;
    return step;
}

using huffman_table = std::array<std::array<huffman_step, 16>, 256>;

// make_huffman_table builds the decoder's table at compile time: what each nibble does in each state.
consteval huffman_table make_huffman_table()
{
    huffman_table table{};
    for (std::size_t state = 0; state < table.size(); ++state)
    {
        for (unsigned nibble = 0; nibble < 16; ++nibble) table[state][nibble] = walk_nibble(state, nibble);
    }
    return table;
}

constexpr huffman_table huffman_decode_table = make_huffman_table();

// static_match is where the static table has a field: its index (1-based), and its name's, or 0 if it hasn't.
struct static_match
{
    std::uint32_t index      = 0;
    std::uint32_t name_index = 0;
};

static_match find_static(std::string_view name, std::string_view value) noexcept
{
    static_match match;
    for (std::uint32_t i = 0; i < static_table.size(); ++i)
    {
        const auto& [n, v] = static_table[i];
        if (n != name) continue;

        if (match.name_index == 0) match.name_index = i + 1;
        if (v == value)
        {
            match.index = i + 1;
            break;
        }
    }

    return match;
}

// encode_string appends s as a string literal: Huffman coded if that's shorter.
void encode_string(std::string_view s, std::vector<std::byte>& out)
{
    // NOTE: a field longer than 4 GiB isn't possible: HTTP/2 limits a header list well before then.
    if (const auto coded = net::http::hpack::huffman_size(s); coded < s.size())
    {
        net::http::hpack::encode_integer(static_cast<std::uint32_t>(coded), 7, 0x80, out);
        net::http::hpack::huffman_encode(s, out);
        return;
    }

    net::http::hpack::encode_integer(static_cast<std::uint32_t>(s.size()), 7, 0x00, out);
    const auto bytes = std::as_bytes(std::span{s});
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// decode_string decodes a string literal from the start of in, advancing past it: as a view of in, or if it's Huffman
// coded, of buffer, which it's decoded into.
std::optional<std::string_view> decode_string(std::span<const std::byte>& in, std::string& buffer)
{
    if (in.empty()) return std::nullopt;

    const bool huffman = (octet(in[0]) & 0x80U) != 0;
    const auto length  = net::http::hpack::decode_integer(in, 7);
    if (!length.has_value() || *length > in.size()) return std::nullopt;

    const auto bytes = in.first(*length);
    in               = in.subspan(*length);

    if (!huffman) return std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};

    buffer.clear();
    if (!net::http::hpack::huffman_decode(bytes, buffer)) return std::nullopt;
    return buffer;
}

}

namespace net::http::hpack
{

bool sensitive(std::string_view name) noexcept
{
    return name == "authorization" || name == "proxy-authorization" || name == "cookie" || name == "set-cookie";
}

field dynamic_table::at(std::size_t index) const noexcept
{
    const auto& e = entries[index];
    return {
        .name  = std::string_view{              e.text.data(),                 e.name_size},
        .value = std::string_view{e.text.data() + e.name_size, e.text.size() - e.name_size}
    };
}

void dynamic_table::insert(std::string_view name, std::string_view value)
{
    const auto size = field_size(name, value);
    if (size > max)
    {
        entries.clear();
        current = 0;
        return;
    }

    // NOTE: copied before evicting, as name or value may be one of the fields evicted.
    entry e;
    e.text.reserve(name.size() + value.size());
    e.text.append(name).append(value);
    e.name_size = name.size();

    evict_to(max - size);
    entries.push_front(std::move(e));
    current += size;
}

void dynamic_table::set_max_size(std::size_t max_size) noexcept
{
    max = max_size;
    evict_to(max);
}

void dynamic_table::evict_to(std::size_t size) noexcept
{
    while (current > size)
    {
        const auto& oldest = entries.back();
        current -= oldest.text.size() + 32;
        entries.pop_back();
    }
}

void decoder::set_max_table_size(std::size_t max_table_size) noexcept
{
    limit = max_table_size;
    if (dynamic.max_size() > limit) update_required = true;
}

std::error_code decoder::decode(std::span<const std::byte> block, callback on_field, void* context)
{
    block_state state{.on_field = on_field, .context = context};

    while (!block.empty())
    {
        const auto first = octet(block[0]);

        bool ok = false;
        if ((first & 0xe0U) == 0x20) ok = decode_size_update(block, state);
        // the peer lowered the limit below the table's size, and didn't say how it shrank it.
        else if (update_required) ok = false;
        else if ((first & 0x80U) != 0) ok = decode_indexed(block, state);
        else ok = decode_literal(block, state);

        if (!ok) return std::make_error_code(std::errc::invalid_argument);
    }

    return state.too_large ? std::make_error_code(std::errc::value_too_large) : std::error_code{};
}

bool decoder::decode_size_update(std::span<const std::byte>& block, const block_state& state) noexcept
{
    // RFC 7541, section 6.3: only before the block's fields.
    if (state.fields) return false;

    const auto size = decode_integer(block, 5);
    if (!size.has_value() || *size > limit) return false;

    dynamic.set_max_size(*size);
    update_required = false;
    return true;
}

bool decoder::decode_indexed(std::span<const std::byte>& block, block_state& state)
{
    // RFC 7541, section 6.1.
    const auto index = decode_integer(block, 7);
    if (!index.has_value()) return false;

    const auto f = lookup(*index);
    if (!f.has_value()) return false;

    emit(*f, state);
    return true;
}

bool decoder::decode_literal(std::span<const std::byte>& block, block_state& state)
{
    // RFC 7541, section 6.2: to be added to the table, or not, or never (by anyone).
    const auto first    = octet(block[0]);
    const bool indexing = (first & 0xc0U) == 0x40;
    const bool never    = (first & 0xf0U) == 0x10;

    const auto name_index = decode_integer(block, indexing ? 6 : 4);
    if (!name_index.has_value()) return false;

    std::optional<std::string_view> name;
    if (*name_index == 0) name = decode_string(block, name_buffer);
    else if (const auto f = lookup(*name_index); f.has_value()) name = f->name;
    if (!name.has_value()) return false;

    const auto value = decode_string(block, value_buffer);
    if (!value.has_value()) return false;

    // NOTE: before inserting, which may evict the field the name is a view of.
    emit({.name = *name, .value = *value, .never_indexed = never}, state);
    if (indexing) dynamic.insert(*name, *value);
    return true;
}

std::optional<field> decoder::lookup(std::uint32_t index) const noexcept
{
    if (index == 0) return std::nullopt;
    if (index <= static_table.size())
    {
        const auto& [name, value] = static_table[index - 1];
        return field{.name = name, .value = value};
    }

    const auto dynamic_index = std::size_t{index} - static_table.size() - 1;
    if (dynamic_index >= dynamic.count()) return std::nullopt;
    return dynamic.at(dynamic_index);
}

void decoder::emit(const field& f, block_state& state) const
{
    state.fields = true;
    state.list_size += field_size(f.name, f.value);
    state.too_large = state.too_large || state.list_size > max_list;
    if (!state.too_large) state.on_field(state.context, f);
}

void encoder::set_max_table_size(std::size_t max_table_size) noexcept
{
    // NOTE: as nothing is added to the table, it only ever needs to shrink.
    if (max_table_size >= table_max) return;

    table_max      = max_table_size;
    pending_update = max_table_size;
}

void encoder::encode(std::span<const field> fields, std::vector<std::byte>& out)
{
    if (pending_update.has_value())
    {
        encode_integer(static_cast<std::uint32_t>(*pending_update), 5, 0x20, out);
        pending_update.reset();
    }

    for (const auto& f : fields)
    {
        // NOTE: a never-indexed field keeps its representation even if the static table has it, so that whoever
        //       decodes it knows not to index it either (RFC 7541, section 6.2.3).
        const auto match = find_static(f.name, f.value);
        if (match.index != 0 && !f.never_indexed)
        {
            encode_integer(match.index, 7, 0x80, out);
            continue;
        }

        // a literal without indexing (0000), or never indexed (0001), with its name's index (or 0, and its name).
        encode_integer(match.name_index, 4, f.never_indexed ? 0x10 : 0x00, out);
        if (match.name_index == 0) encode_string(f.name, out);
        encode_string(f.value, out);
    }
}

void encode_integer(std::uint32_t value, int prefix_bits, std::uint8_t first_bits, std::vector<std::byte>& out)
{
    const std::uint32_t prefix_max = (1U << prefix_bits) - 1;
    if (value < prefix_max)
    {
        out.push_back(static_cast<std::byte>(first_bits | value));
        return;
    }

    out.push_back(static_cast<std::byte>(first_bits | prefix_max));
    value -= prefix_max;
    while (value >= 0x80)
    {
        out.push_back(static_cast<std::byte>((value & 0x7fU) | 0x80U));
        value >>= 7;
    }
    out.push_back(static_cast<std::byte>(value));
}

std::optional<std::uint32_t> decode_integer(std::span<const std::byte>& in, int prefix_bits) noexcept
{
    if (in.empty()) return std::nullopt;

    const std::uint32_t prefix_max = (1U << prefix_bits) - 1;
    std::uint64_t       value      = octet(in[0]) & prefix_max;
    std::size_t         used       = 1;

    if (value == prefix_max)
    {
        // at most 5 continuation octets (35 bits), so neither the value nor the octets spent on it are unbounded.
        for (int shift = 0;; shift += 7)
        {
            if (used == in.size() || shift > 28) return std::nullopt;

            const auto b = octet(in[used++]);
            value += std::uint64_t{b & 0x7fU} << shift;
            if ((b & 0x80U) == 0) break;
        }

        if (value > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    }

    in = in.subspan(used);
    return static_cast<std::uint32_t>(value);
}

std::size_t huffman_size(std::string_view s) noexcept
{
    std::size_t bits = 0;
    for (const auto c : s) bits += huffman_codes[static_cast<unsigned char>(c)].second;
    return (bits + 7) / 8;
}

void huffman_encode(std::string_view s, std::vector<std::byte>& out)
{
    // bits not written yet are the lowest count bits of pending (at most 7, plus a code's 30).
    std::uint64_t pending = 0;
    int           count   = 0;

    for (const auto c : s)
    {
        const auto [code, bits] = huffman_codes[static_cast<unsigned char>(c)];
        pending                 = (pending << bits) | code;
        count += bits;

        while (count >= 8)
        {
            count -= 8;
            out.push_back(static_cast<std::byte>(pending >> count));
        }
    }

    // padded with EOS's leading bits (all ones).
    if (count > 0) out.push_back(static_cast<std::byte>((pending << (8 - count)) | (0xffU >> count)));
}

bool huffman_decode(std::span<const std::byte> in, std::string& out)
{
    // the shortest code is 5 bits.
    out.reserve(out.size() + (in.size() * 8 / 5));

    std::uint8_t state  = 0;
    bool         accept = true;

    for (const auto b : in)
    {
        const auto o = octet(b);
        for (const unsigned nibble : {static_cast<unsigned>(o >> 4U), o & 0xfU})
        {
            const auto& step = huffman_decode_table[state][nibble];
            if ((step.flags & fails) != 0) return false;
            if ((step.flags & emits) != 0) out.push_back(static_cast<char>(step.symbol));

            state  = step.next;
            accept = (step.flags & accepting) != 0;
        }
    }

    return accept;
}

}
