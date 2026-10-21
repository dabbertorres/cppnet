#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// HPACK (RFC 7541): HTTP/2's header compression. It's what HTTP/2 is built from, not for applications (see
// docs/plans/http2.md).
namespace net::http::hpack
{

// field is a header field as HPACK encodes and decodes it. never_indexed marks one that must never be added to a
// dynamic table, by this side or by an intermediary that re-encodes it: e.g. a credential (see sensitive()).
struct field
{
    std::string_view name;
    std::string_view value;
    bool             never_indexed = false;

    bool operator==(const field&) const = default;
};

// sensitive is whether a field named name should never be indexed: credentials and cookies, which could otherwise be
// guessed at by an attacker who can add fields and watch the compressed size (RFC 7541, section 7.1). name is
// lowercase, as HTTP/2's are.
[[nodiscard]] bool sensitive(std::string_view name) noexcept;

// field_size is what a field counts towards a dynamic table's size, and a header list's: its name's and value's
// lengths, and 32 (RFC 7541, section 4.1).
[[nodiscard]] constexpr std::size_t field_size(std::string_view name, std::string_view value) noexcept
{
    return name.size() + value.size() + 32;
}

// default_table_size is SETTINGS_HEADER_TABLE_SIZE's initial value: a dynamic table's size until the peer says
// otherwise.
inline constexpr std::size_t default_table_size = 4'096;

// default_max_list_size is the most a decoded header list may come to (by field_size), unless a decoder is told
// otherwise: as the fix for CVE-2016-6581 (an HPACK bomb, which expands about 4096 to 1) chose.
inline constexpr std::size_t default_max_list_size = std::size_t{64} * 1'024;

// dynamic_table holds the fields an encoder or decoder has indexed, newest first, evicting the oldest to stay within
// its maximum size (RFC 7541, section 4).
//
// Its storage is made once rather than per field (docs/plans/http2.md, Decided question 16 of stage 5): the fields'
// text is appended to one buffer, oldest first, and moved to the buffer's front when it reaches the end (the buffer
// being up to twice the maximum size, what's moved is at most the maximum size, for as much appended); and each field
// is a fixed-size entry in a ring (its text's offset, its name's and value's lengths and hashes), of which there are
// at most a 32nd of the maximum size. Both grow, by doubling, only as the fields need them, up to those bounds: a
// table nothing's added to allocates nothing, and one whose maximum size is large (a peer's SETTINGS) holds only what
// it's given.
class dynamic_table
{
public:
    // npos is no index: what find() gives when there's no such field.
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    // match is what find() found: the index of a field with the name and value (or npos), and of the newest with the
    // name (or npos).
    struct match
    {
        std::size_t index      = npos;
        std::size_t name_index = npos;
    };

    // max_size is limited to what HPACK's integers can say (SETTINGS_HEADER_TABLE_SIZE is 32 bits), as are a field's
    // lengths.
    explicit dynamic_table(std::size_t max_size = default_table_size) noexcept
        : max{limited(max_size)}
    {}

    // size is what the fields come to (by field_size).
    [[nodiscard]] std::size_t size() const noexcept { return current; }
    [[nodiscard]] std::size_t max_size() const noexcept { return max; }
    [[nodiscard]] std::size_t count() const noexcept { return entries; }

    // at is the field at index (0 being the newest), which must be less than count(). Its views are valid until the
    // table next changes.
    [[nodiscard]] field at(std::size_t index) const noexcept;

    // find goes through the fields, newest first, for one with name and value, comparing their hashes and lengths
    // before their text.
    [[nodiscard]] match find(std::string_view name, std::string_view value) const noexcept;

    // insert adds a field (copying it, so it may be one of the table's own), evicting the oldest until it fits. A field
    // larger than the maximum size empties the table, and isn't added.
    void insert(std::string_view name, std::string_view value);

    // set_max_size changes the maximum size, evicting the oldest fields until they fit.
    void set_max_size(std::size_t max_size) noexcept;

private:
    // entry is where a field is: its text (the name, then the value) at offset in text.
    struct entry
    {
        std::size_t   offset     = 0;
        std::uint32_t name_size  = 0;
        std::uint32_t value_size = 0;
        std::uint32_t name_hash  = 0;
        std::uint32_t value_hash = 0;
    };

    static constexpr std::size_t limited(std::size_t size) noexcept
    {
        return size < std::numeric_limits<std::uint32_t>::max() ? size : std::numeric_limits<std::uint32_t>::max();
    }

    // nth is the entry at index (0 being the newest).
    [[nodiscard]] const entry& nth(std::size_t index) const noexcept
    {
        return ring[(first + entries - 1 - index) & (ring.size() - 1)];
    }

    // make_room makes room for length more bytes of text after the newest field's: moving the fields' text to the front
    // of the buffer, or into a larger one, and name and value with it if they're views of a field's.
    void make_room(std::size_t length, std::string_view& name, std::string_view& value);

    void push(const entry& e);
    void evict_to(std::size_t size) noexcept;

    std::vector<char>  text; // the fields' text, the oldest's from its offset, the newest's ending at end
    std::vector<entry> ring; // the fields, the oldest at first; its size a power of two (or 0)
    std::size_t        first   = 0;
    std::size_t        entries = 0;
    std::size_t        end     = 0;
    std::size_t        current = 0;
    std::size_t        max;
};

// decoder decodes header blocks: one per connection, as the dynamic table is the connection's.
class decoder
{
public:
    // max_table_size is the dynamic table's size to start with, and the limit a size update mustn't exceed: the
    // SETTINGS_HEADER_TABLE_SIZE both sides start with (as the peer can't have seen this side's yet), which
    // set_max_table_size() changes. max_list_size is the most a block's header list may come to.
    explicit decoder(std::size_t max_table_size = default_table_size,
                     std::size_t max_list_size  = default_max_list_size) noexcept
        : dynamic{max_table_size}
        , limit{max_table_size}
        , max_list{max_list_size}
    {}

    // set_max_table_size changes the limit, once the peer has acknowledged a SETTINGS_HEADER_TABLE_SIZE. If it's less
    // than the table's maximum size, the next block must start with a size update to within it (RFC 9113, section
    // 4.3.1).
    void set_max_table_size(std::size_t max_table_size) noexcept;

    // decode decodes a header block (from a HEADERS frame and its CONTINUATIONs), calling on_field(const field&) for
    // each field, in order. The field's views are only valid during the call. It fails with:
    // - std::errc::invalid_argument if the block is malformed: a COMPRESSION_ERROR, which ends the connection.
    // - std::errc::value_too_large if the header list comes to more than max_list_size (by field_size). Decoding goes
    //   on to the end, without calling on_field, so that the dynamic table stays in step with the encoder's, and the
    //   connection can go on (refusing the stream).
    template<typename F>
    std::error_code decode(std::span<const std::byte> block, F on_field)
    {
        return decode(
            block,
            [](void* context, const field& f) { (*static_cast<F*>(context))(f); },
            static_cast<void*>(&on_field));
    }

    [[nodiscard]] const dynamic_table& table() const noexcept { return dynamic; }

private:
    using callback = void (*)(void* context, const field& f);

    std::error_code decode(std::span<const std::byte> block, callback on_field, void* context);

    // block_state is what decoding a block keeps track of.
    struct block_state
    {
        callback    on_field  = nullptr;
        void*       context   = nullptr;
        std::size_t list_size = 0;
        bool        too_large = false;
        bool        fields    = false; // whether a field has been decoded: size updates must come first
    };

    // Each decodes a representation from the start of block, advancing past it, returning false if it's malformed.
    bool decode_size_update(std::span<const std::byte>& block, const block_state& state) noexcept;
    bool decode_indexed(std::span<const std::byte>& block, block_state& state);
    bool decode_literal(std::span<const std::byte>& block, block_state& state);

    // lookup is the field at index: 1-based, in the static table, then the dynamic table.
    [[nodiscard]] std::optional<field> lookup(std::uint32_t index) const noexcept;

    // emit passes a decoded field on, unless the header list has become too large.
    void emit(const field& f, block_state& state) const;

    dynamic_table dynamic;
    std::size_t   limit;
    std::size_t   max_list;
    bool          update_required = false;

    // for strings that were Huffman coded, reused from one field to the next.
    std::string name_buffer;
    std::string value_buffer;
};

// indexing is what an encoder adds to its dynamic table (docs/plans/http2.md, "HPACK's dynamic table, for the
// encoder", and Decided questions 13 and 14 of stage 5).
enum class indexing : std::uint8_t
{
    // none adds nothing: a field the static table has is encoded as its index (unless it's never indexed, whose
    // representation must say so), and others as literals without indexing (or never indexed), with the static table's
    // index for the name, if it has the name. The table only ever shrinks (it's empty, so growing it would gain
    // nothing), and only what field::never_indexed says is never indexed.
    none,

    // automatic adds what's likely to repeat, nghttp2's way: everything but
    // - never-indexed fields: field::never_indexed, and what sensitive() names (sent as never indexed, whatever the
    //   field says, so that intermediaries don't index them either);
    // - fields that change from one message to the next (:path, age, content-length, etag, if-modified-since,
    //   if-none-match, location), whose entries would only push out others: literals without indexing;
    // - and a field that would take more than 3/4 of the table, likewise.
    // A field is encoded as the first of: an index of the static table's, or the dynamic table's (unless it's never
    // indexed), then a literal with the static table's index for the name, or the dynamic table's (Go's order).
    automatic,
};

// encoder encodes header blocks: one per connection, as the dynamic table is the connection's, kept in step with the
// peer's decoder's.
class encoder
{
public:
    explicit encoder(indexing indexing_mode = indexing::none) noexcept
        : mode{indexing_mode}
    {}

    // set_max_table_size takes the peer's SETTINGS_HEADER_TABLE_SIZE, each time it changes (with indexing::none, only
    // if it shrinks the table). The next block starts with the smallest size the table has had since the last one, if
    // that's below both what the peer's decoder knows and the size it has now (it may have evicted fields there), then
    // the size it has now, if that, or the smallest, differs from what the decoder knows (RFC 7541, section 4.2); as
    // RFC 9113, section 4.3.1, requires when the peer lowers its setting.
    void set_max_table_size(std::size_t max_table_size) noexcept;

    // encode appends a block of fields to out.
    void encode(std::span<const field> fields, std::vector<std::byte>& out);

    [[nodiscard]] std::size_t          max_table_size() const noexcept { return dynamic.max_size(); }
    [[nodiscard]] const dynamic_table& table() const noexcept { return dynamic; }

private:
    // unchanged is smallest's value when the table's size hasn't changed since the last block.
    static constexpr std::size_t unchanged = std::numeric_limits<std::size_t>::max();

    void encode_field(const field& f, std::vector<std::byte>& out);

    dynamic_table dynamic;
    std::size_t   known    = default_table_size; // the table's size as the peer's decoder knows it
    std::size_t   smallest = unchanged;          // since the last block
    indexing      mode;
};

// The building blocks, for HTTP/2's code and its tests.

// encode_integer appends value with an n-bit prefix (1 to 8) to out, whose first octet's other bits are first_bits
// (RFC 7541, section 5.1).
void encode_integer(std::uint32_t value, int prefix_bits, std::uint8_t first_bits, std::vector<std::byte>& out);

// decode_integer decodes an integer with an n-bit prefix from the start of in, advancing past it. It fails (nullopt)
// if in ends first, or the integer doesn't fit in 32 bits (or takes more octets than one that does).
[[nodiscard]] std::optional<std::uint32_t> decode_integer(std::span<const std::byte>& in, int prefix_bits) noexcept;

// huffman_size is how many octets huffman_encode makes of s.
[[nodiscard]] std::size_t huffman_size(std::string_view s) noexcept;

// huffman_encode appends s's Huffman coding (RFC 7541, Appendix B) to out, padded with EOS's leading bits.
void huffman_encode(std::string_view s, std::vector<std::byte>& out);

// huffman_decode appends what in decodes to to out, returning false if it isn't a valid coding: it includes EOS, or
// is padded with more than 7 bits, or with bits that aren't EOS's.
[[nodiscard]] bool huffman_decode(std::span<const std::byte> in, std::string& out);

}
