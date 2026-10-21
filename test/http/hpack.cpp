#include "http/hpack.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "encoding/hex.hpp"
#include "http/hpack_stories.hpp"

namespace hpack = net::http::hpack;

using namespace std::string_view_literals;

namespace
{

using field_list = std::vector<std::pair<std::string_view, std::string_view>>;

struct example
{
    std::string_view encoded; // hex
    field_list       fields;
    field_list       table; // newest first
    std::size_t      table_size = 0;
};

struct sequence
{
    std::string_view     name;
    std::size_t          table_size = hpack::default_table_size;
    std::vector<example> examples;
};

// RFC 7541, Appendix C (generated from its text by tools/hpack/gen_examples.py): each sequence is decoded in
// order by one decoder.
std::vector<sequence> rfc_examples()
{
    return {
        sequence{
                 .name       = "C.2.1: a literal with indexing",
                 .table_size = 4096,
                 .examples =
                 {
                 // C.2.1. Literal Header Field with Indexing
                 example{
                 .encoded    = "400a637573746f6d2d6b65790d637573746f6d2d686561646572",
                 .fields     = {{"custom-key", "custom-header"}},
                 .table      = {{"custom-key", "custom-header"}},
                 .table_size = 55,
                 },
                 }, },
        sequence{
                 .name       = "C.2.2: a literal without indexing",
                 .table_size = 4096,
                 .examples =
                 {
                 // C.2.2. Literal Header Field without Indexing
                 example{
                 .encoded    = "040c2f73616d706c652f70617468",
                 .fields     = {{":path", "/sample/path"}},
                 .table      = {},
                 .table_size = 0,
                 },
                 }, },
        sequence{
                 .name       = "C.2.3: a literal never indexed",
                 .table_size = 4096,
                 .examples =
                 {
                 // C.2.3. Literal Header Field Never Indexed
                 example{
                 .encoded    = "100870617373776f726406736563726574",
                 .fields     = {{"password", "secret"}},
                 .table      = {},
                 .table_size = 0,
                 },
                 }, },
        sequence{
                 .name       = "C.2.4: an indexed field",
                 .table_size = 4096,
                 .examples =
                 {
                 // C.2.4. Indexed Header Field
                 example{
                 .encoded    = "82",
                 .fields     = {{":method", "GET"}},
                 .table      = {},
                 .table_size = 0,
                 },
                 }, },
        sequence{
                 .name       = "C.3: requests without Huffman coding",
                 .table_size = 4096,
                 .examples =
                 {
                 // C.3.1. First Request
                 example{
                 .encoded    = "828684410f7777772e6578616d706c652e636f6d",
                 .fields     = {{":method", "GET"},
                 {":scheme", "http"},
                 {":path", "/"},
                 {":authority", "www.example.com"}},
                 .table      = {{":authority", "www.example.com"}},
                 .table_size = 57,
                 },
                 // C.3.2. Second Request
                 example{
                 .encoded    = "828684be58086e6f2d6361636865",
                 .fields     = {{":method", "GET"},
                 {":scheme", "http"},
                 {":path", "/"},
                 {":authority", "www.example.com"},
                 {"cache-control", "no-cache"}},
                 .table      = {{"cache-control", "no-cache"}, {":authority", "www.example.com"}},
                 .table_size = 110,
                 },
                 // C.3.3. Third Request
                 example{
                 .encoded    = "828785bf400a637573746f6d2d6b65790c637573746f6d2d76616c7565",
                 .fields     = {{":method", "GET"},
                 {":scheme", "https"},
                 {":path", "/index.html"},
                 {":authority", "www.example.com"},
                 {"custom-key", "custom-value"}},
                 .table      = {{"custom-key", "custom-value"},
                 {"cache-control", "no-cache"},
                 {":authority", "www.example.com"}},
                 .table_size = 164,
                 },
                 }, },
        sequence{
                 .name       = "C.4: requests with Huffman coding",
                 .table_size = 4096,
                 .examples =
                 {
                 // C.4.1. First Request
                 example{
                 .encoded    = "828684418cf1e3c2e5f23a6ba0ab90f4ff",
                 .fields     = {{":method", "GET"},
                 {":scheme", "http"},
                 {":path", "/"},
                 {":authority", "www.example.com"}},
                 .table      = {{":authority", "www.example.com"}},
                 .table_size = 57,
                 },
                 // C.4.2. Second Request
                 example{
                 .encoded    = "828684be5886a8eb10649cbf",
                 .fields     = {{":method", "GET"},
                 {":scheme", "http"},
                 {":path", "/"},
                 {":authority", "www.example.com"},
                 {"cache-control", "no-cache"}},
                 .table      = {{"cache-control", "no-cache"}, {":authority", "www.example.com"}},
                 .table_size = 110,
                 },
                 // C.4.3. Third Request
                 example{
                 .encoded    = "828785bf408825a849e95ba97d7f8925a849e95bb8e8b4bf",
                 .fields     = {{":method", "GET"},
                 {":scheme", "https"},
                 {":path", "/index.html"},
                 {":authority", "www.example.com"},
                 {"custom-key", "custom-value"}},
                 .table      = {{"custom-key", "custom-value"},
                 {"cache-control", "no-cache"},
                 {":authority", "www.example.com"}},
                 .table_size = 164,
                 },
                 }, },
        sequence{
                 .name       = "C.5: responses without Huffman coding, evicting",
                 .table_size = 256,
                 .examples =
                 {
                 // C.5.1. First Response
                 example{
                 .encoded =
                 "4803333032580770726976617465611d4d6f6e2c203231204f637420323031332032303a31333a323120474d546e1768747470733a2f2f7777772e6578616d706c652e636f6d",
                 .fields     = {{":status", "302"},
                 {"cache-control", "private"},
                 {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                 {"location", "https://www.example.com"}},
                 .table      = {{"location", "https://www.example.com"},
                 {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                 {"cache-control", "private"},
                 {":status", "302"}},
                 .table_size = 222,
                 },
                 // C.5.2. Second Response
                 example{
                 .encoded    = "4803333037c1c0bf",
                 .fields     = {{":status", "307"},
                 {"cache-control", "private"},
                 {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                 {"location", "https://www.example.com"}},
                 .table      = {{":status", "307"},
                 {"location", "https://www.example.com"},
                 {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                 {"cache-control", "private"}},
                 .table_size = 222,
                 },
                 // C.5.3. Third Response
                 example{
                 .encoded =
                 "88c1611d4d6f6e2c203231204f637420323031332032303a31333a323220474d54c05a04677a69707738666f6f3d4153444a4b48514b425a584f5157454f50495541585157454f49553b206d61782d6167653d333630303b2076657273696f6e3d31",
                 .fields     = {{":status", "200"},
                 {"cache-control", "private"},
                 {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
                 {"location", "https://www.example.com"},
                 {"content-encoding", "gzip"},
                 {"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"}},
                 .table      = {{"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"},
                 {"content-encoding", "gzip"},
                 {"date", "Mon, 21 Oct 2013 20:13:22 GMT"}},
                 .table_size = 215,
                 },
                 }, },
        sequence{
                 .name       = "C.6: responses with Huffman coding, evicting",
                 .table_size = 256,
                 .examples =
                 {
                 // C.6.1. First Response
                 example{
                 .encoded =
                 "488264025885aec3771a4b6196d07abe941054d444a8200595040b8166e082a62d1bff6e919d29ad171863c78f0b97c8e9ae82ae43d3",
                 .fields     = {{":status", "302"},
                 {"cache-control", "private"},
                 {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                 {"location", "https://www.example.com"}},
                 .table      = {{"location", "https://www.example.com"},
                 {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                 {"cache-control", "private"},
                 {":status", "302"}},
                 .table_size = 222,
                 },
                 // C.6.2. Second Response
                 example{
                 .encoded    = "4883640effc1c0bf",
                 .fields     = {{":status", "307"},
                 {"cache-control", "private"},
                 {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                 {"location", "https://www.example.com"}},
                 .table      = {{":status", "307"},
                 {"location", "https://www.example.com"},
                 {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                 {"cache-control", "private"}},
                 .table_size = 222,
                 },
                 // C.6.3. Third Response
                 example{
                 .encoded =
                 "88c16196d07abe941054d444a8200595040b8166e084a62d1bffc05a839bd9ab77ad94e7821dd7f2e6c7b335dfdfcd5b3960d5af27087f3672c1ab270fb5291f9587316065c003ed4ee5b1063d5007",
                 .fields     = {{":status", "200"},
                 {"cache-control", "private"},
                 {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
                 {"location", "https://www.example.com"},
                 {"content-encoding", "gzip"},
                 {"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"}},
                 .table      = {{"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"},
                 {"content-encoding", "gzip"},
                 {"date", "Mon, 21 Oct 2013 20:13:22 GMT"}},
                 .table_size = 215,
                 },
                 }, },
    };
}

std::vector<std::byte> from_hex(std::string_view hex)
{
    auto bytes = net::encoding::hex::decode(hex);
    REQUIRE(bytes.has_value());
    return *bytes;
}

std::string to_hex(std::span<const std::byte> bytes)
{
    auto hex = net::encoding::hex::encode(bytes);
    for (auto& c : hex) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return hex;
}

// decoded is what decoding a block made: its fields (copied), and the error, if any.
struct decoded
{
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<bool>                                never_indexed;
    std::error_code                                  err;
};

decoded decode(hpack::decoder& dec, std::span<const std::byte> block)
{
    decoded out;
    out.err = dec.decode(block,
                         [&](const hpack::field& f)
                         {
                             out.fields.emplace_back(f.name, f.value);
                             out.never_indexed.push_back(f.never_indexed);
                         });
    return out;
}

field_list table_of(const hpack::dynamic_table& table)
{
    field_list out;
    for (std::size_t i = 0; i < table.count(); ++i)
    {
        const auto f = table.at(i);
        out.emplace_back(f.name, f.value);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> owned(const field_list& fields)
{
    return {fields.begin(), fields.end()};
}

}

TEST_CASE("integers are encoded with a prefix, and continue in 7-bit octets", "[http][hpack][integer]")
{
    struct test_case
    {
        std::uint32_t    value;
        int              prefix_bits;
        std::string_view hex;
    };

    // RFC 7541, Appendix C.1, and the edges.
    const auto tc = GENERATE(test_case{.value = 10, .prefix_bits = 5, .hex = "0a"},
                             test_case{.value = 1337, .prefix_bits = 5, .hex = "1f9a0a"},
                             test_case{.value = 42, .prefix_bits = 8, .hex = "2a"},
                             test_case{.value = 30, .prefix_bits = 5, .hex = "1e"},
                             test_case{.value = 31, .prefix_bits = 5, .hex = "1f00"},
                             test_case{.value = 127, .prefix_bits = 7, .hex = "7f00"},
                             test_case{.value = 0xffff'ffff, .prefix_bits = 1, .hex = "01feffffff0f"});
    CAPTURE(tc.value, tc.prefix_bits);

    std::vector<std::byte> out;
    hpack::encode_integer(tc.value, tc.prefix_bits, 0, out);
    CHECK(to_hex(out) == tc.hex);

    // with what's after it left alone.
    out.push_back(std::byte{0x55});
    std::span<const std::byte> in{out};
    CHECK(hpack::decode_integer(in, tc.prefix_bits) == tc.value);
    CHECK(in.size() == 1);
}

TEST_CASE("integers that don't fit in 32 bits, or don't end, aren't decoded", "[http][hpack][integer]")
{
    const auto hex = GENERATE("1f"sv,           // ends before its continuation
                              "1f80"sv,         // ends during it
                              "01ffffffff0f"sv, // 2^32: too large
                              "1f8080808080"sv, // too many octets, even for a small value
                              ""sv);
    CAPTURE(hex);

    const auto                 bytes = from_hex(hex);
    std::span<const std::byte> in{bytes};
    CHECK_FALSE(hpack::decode_integer(in, hex.starts_with("01") ? 1 : 5).has_value());
}

TEST_CASE("Huffman coding matches the RFC's examples, and round-trips every octet", "[http][hpack][huffman]")
{
    SECTION("the RFC's examples (Appendix C.4 and C.6)")
    {
        const auto [text, hex] =
            GENERATE(std::pair{"www.example.com"sv, "f1e3c2e5f23a6ba0ab90f4ff"sv},
                     std::pair{"no-cache"sv, "a8eb10649cbf"sv},
                     std::pair{"custom-key"sv, "25a849e95ba97d7f"sv},
                     std::pair{"custom-value"sv, "25a849e95bb8e8b4bf"sv},
                     std::pair{"302"sv, "6402"sv},
                     std::pair{"Mon, 21 Oct 2013 20:13:21 GMT"sv, "d07abe941054d444a8200595040b8166e082a62d1bff"sv});
        CAPTURE(text);

        std::vector<std::byte> out;
        hpack::huffman_encode(text, out);
        CHECK(to_hex(out) == hex);
        CHECK(hpack::huffman_size(text) == out.size());

        std::string decoded;
        CHECK(hpack::huffman_decode(out, decoded));
        CHECK(decoded == text);
    }

    SECTION("every octet")
    {
        std::string all;
        for (int i = 0; i < 256; ++i) all.push_back(static_cast<char>(i));

        std::vector<std::byte> out;
        hpack::huffman_encode(all, out);
        CHECK(hpack::huffman_size(all) == out.size());

        std::string decoded;
        CHECK(hpack::huffman_decode(out, decoded));
        CHECK(decoded == all);
    }

    SECTION("the empty string")
    {
        std::string decoded;
        CHECK(hpack::huffman_decode({}, decoded));
        CHECK(decoded.empty());
    }
}

TEST_CASE("Huffman codings with EOS, or padding that isn't EOS's, are rejected", "[http][hpack][huffman]")
{
    const auto hex = GENERATE("ffffffff"sv, // EOS (30 ones), and 2 more
                              "1fff"sv,     // 'a' (00011), then 11 ones of padding: more than 7
                              "18"sv,       // 'a', then 000: padding that isn't ones
                              "1f00"sv);    // 'a', then a whole octet of zeros
    CAPTURE(hex);

    std::string decoded;
    CHECK_FALSE(hpack::huffman_decode(from_hex(hex), decoded));
}

TEST_CASE("the RFC's examples decode as it says, with its dynamic tables", "[http][hpack][decoder]")
{
    for (const auto& seq : rfc_examples())
    {
        CAPTURE(seq.name);

        hpack::decoder dec{seq.table_size};
        for (const auto& ex : seq.examples)
        {
            CAPTURE(ex.encoded);

            const auto got = decode(dec, from_hex(ex.encoded));
            CHECK_FALSE(got.err);
            CHECK(got.fields == owned(ex.fields));
            CHECK(table_of(dec.table()) == ex.table);
            CHECK(dec.table().size() == ex.table_size);
        }
    }
}

TEST_CASE("hpack-test-case's stories, as other implementations encoded them, decode to their headers",
          "[http][hpack][decoder]")
{
    for (const auto& story : hpack_stories::stories)
    {
        CAPTURE(story.name);

        hpack::decoder dec;
        auto           fields = story.fields;
        for (const auto& c : story.cases)
        {
            CAPTURE(c.wire);
            REQUIRE(c.fields <= fields.size());

            // SETTINGS_HEADER_TABLE_SIZE, acknowledged.
            if (c.header_table_size != 0) dec.set_max_table_size(c.header_table_size);

            std::vector<std::pair<std::string, std::string>> want;
            for (const auto& f : fields.first(c.fields)) want.emplace_back(f.name, f.value);
            fields = fields.subspan(c.fields);

            const auto got = decode(dec, from_hex(c.wire));
            CHECK_FALSE(got.err);
            CHECK(got.fields == want);
        }
        CHECK(fields.empty());
    }
}

TEST_CASE("a never-indexed literal says so", "[http][hpack][decoder]")
{
    // RFC 7541, Appendix C.2.3: "password: secret", never indexed.
    hpack::decoder dec;
    const auto     got = decode(dec, from_hex("100870617373776f726406736563726574"));
    REQUIRE_FALSE(got.err);
    CHECK(got.never_indexed == std::vector<bool>{true});
}

TEST_CASE("malformed header blocks are compression errors", "[http][hpack][decoder]")
{
    struct test_case
    {
        std::string_view what;
        std::string_view hex;
    };

    const auto tc =
        GENERATE(test_case{.what = "index 0", .hex = "80"},
                 test_case{.what = "an index past the static table, with the dynamic table empty", .hex = "be"},
                 test_case{.what = "an index that doesn't end", .hex = "ff"},
                 test_case{.what = "a literal name's index past the tables", .hex = "7f0a0161"},
                 test_case{.what = "a literal that ends before its name", .hex = "40"},
                 test_case{.what = "a name longer than the block", .hex = "4005616263"},
                 test_case{.what = "a value longer than the block", .hex = "0405616263"},
                 test_case{.what = "a value that isn't valid Huffman coding", .hex = "0481ff"},
                 test_case{.what = "a size update after a field", .hex = "8220"},
                 test_case{.what = "a size update past the limit", .hex = "3fe21f"});
    CAPTURE(tc.what);

    hpack::decoder dec;
    CHECK(decode(dec, from_hex(tc.hex)).err == std::errc::invalid_argument);
}

TEST_CASE("dynamic table size updates come first, within the limit, and are required after it's lowered",
          "[http][hpack][decoder]")
{
    hpack::decoder dec;

    // C.3.1's request, which adds ":authority: www.example.com" (57).
    REQUIRE_FALSE(decode(dec, from_hex("828684410f7777772e6578616d706c652e636f6d")).err);
    REQUIRE(dec.table().size() == 57);

    SECTION("a size update shrinks the table, evicting")
    {
        // to 0, then back to 4096: two updates in a row are allowed.
        CHECK_FALSE(decode(dec, from_hex("203fe11f82")).err);
        CHECK(dec.table().count() == 0);
        CHECK(dec.table().max_size() == 4'096);
    }

    SECTION("once the limit is lowered, the next block must start with an update")
    {
        dec.set_max_table_size(100);
        CHECK(decode(dec, from_hex("82")).err == std::errc::invalid_argument);
    }

    SECTION("an update within the new limit satisfies it")
    {
        dec.set_max_table_size(100);
        CHECK_FALSE(decode(dec, from_hex("3f4582")).err); // 100, then :method: GET
        CHECK(dec.table().max_size() == 100);
        CHECK(dec.table().size() == 57);

        CHECK_FALSE(decode(dec, from_hex("82")).err);
    }

    SECTION("raising the limit needs no update")
    {
        dec.set_max_table_size(8'192);
        CHECK_FALSE(decode(dec, from_hex("82")).err);
    }
}

TEST_CASE("a header list over the limit is decoded to the end, without its fields, keeping the table in step",
          "[http][hpack][decoder]")
{
    hpack::decoder dec{hpack::default_table_size, 200};

    SECTION("a field that's too large")
    {
        // "x: " 200 a's, with indexing (233 bytes, by field_size).
        std::vector<std::byte> block{std::byte{0x40}, std::byte{0x01}, std::byte{'x'}};
        hpack::encode_integer(200, 7, 0, block);
        block.insert(block.end(), 200, std::byte{'a'});

        const auto got = decode(dec, block);
        CHECK(got.err == std::errc::value_too_large);
        CHECK(got.fields.empty());
        CHECK(dec.table().count() == 1);
    }

    SECTION("an HPACK bomb: one large entry, referred to again and again")
    {
        hpack::decoder bombed{hpack::default_table_size, hpack::default_max_list_size};

        // a 4000-byte entry (about the whole table), then 10000 references to it: 40 MB if decoded.
        std::vector<std::byte> block{std::byte{0x40}, std::byte{0x01}, std::byte{'x'}};
        hpack::encode_integer(4'000 - 33, 7, 0, block);
        block.insert(block.end(), 4'000 - 33, std::byte{'a'});
        block.insert(block.end(), 10'000, std::byte{0xbe}); // index 62: the newest entry

        std::size_t called = 0;
        const auto  err    = bombed.decode(block, [&](const hpack::field&) { ++called; });
        CHECK(err == std::errc::value_too_large);
        CHECK(called == hpack::default_max_list_size / 4'000);
    }
}

TEST_CASE("the dynamic table evicts the oldest fields, and empties for one too large", "[http][hpack][table]")
{
    hpack::dynamic_table table{100};
    table.insert("a", "1"); // 34
    table.insert("b", "2"); // 34
    REQUIRE(table.size() == 68);

    SECTION("evicting the oldest")
    {
        table.insert("c", "3");
        CHECK(table_of(table)
              == field_list{
                  {"c", "3"},
                  {"b", "2"}
        });
        CHECK(table.size() == 68);
    }

    SECTION("inserting a field named from one it evicts")
    {
        // "b" is evicted to make room for a field whose name is a view of it.
        const auto name = table.at(0).name;
        table.insert(name, std::string(60, 'v')); // 93
        CHECK(table_of(table)
              == field_list{
                  {"b", std::string_view{std::string(60, 'v')}}
        });
    }

    SECTION("a field larger than the table empties it")
    {
        table.insert("big", std::string(100, 'v'));
        CHECK(table.count() == 0);
        CHECK(table.size() == 0);
    }

    SECTION("shrinking evicts")
    {
        table.set_max_size(40);
        CHECK(table_of(table)
              == field_list{
                  {"b", "2"}
        });
    }
}

TEST_CASE("encoded fields decode to what was encoded", "[http][hpack][encoder]")
{
    const std::vector<hpack::field> fields{
        {.name = ":method", .value = "GET"},
        {.name = ":path", .value = "/a/long/path/that/huffman/makes/shorter"},
        {.name = ":authority", .value = "example.com"},
        {.name = "accept-encoding", .value = "gzip, deflate"},
        {.name = "x-custom", .value = "\x01\x02\x03"},
        {.name = "authorization", .value = "Bearer secret", .never_indexed = true},
        {.name = "empty", .value = ""},
    };

    hpack::encoder         enc;
    std::vector<std::byte> block;
    enc.encode(fields, block);

    hpack::decoder            dec;
    std::vector<hpack::field> got;
    std::vector<std::string>  storage;
    storage.reserve(fields.size() * 2);
    const auto err =
        dec.decode(block,
                   [&](const hpack::field& f)
                   {
                       storage.emplace_back(f.name);
                       const std::string_view name = storage.back();
                       storage.emplace_back(f.value);
                       got.push_back({.name = name, .value = storage.back(), .never_indexed = f.never_indexed});
                   });
    REQUIRE_FALSE(err);
    CHECK(got == fields);

    // nothing's added to the table.
    CHECK(dec.table().count() == 0);
}

TEST_CASE("the encoder chooses the shortest representation it has", "[http][hpack][encoder]")
{
    hpack::encoder         enc;
    std::vector<std::byte> out;

    SECTION("a field the static table has is its index")
    {
        const std::vector<hpack::field> fields{
            {.name = ":method", .value = "GET"},
            {.name = ":status", .value = "404"}
        };
        enc.encode(fields, out);
        CHECK(to_hex(out) == "828d");
    }

    SECTION("a name the static table has is its index, with the value a literal")
    {
        // ":path" is index 4: a literal without indexing (0000), with a raw value (Huffman coding wouldn't be shorter).
        const std::vector<hpack::field> fields{
            {.name = ":path", .value = "/~"}
        };
        enc.encode(fields, out);
        CHECK(to_hex(out) == "04022f7e");
    }

    SECTION("a never-indexed field says so")
    {
        const std::vector<hpack::field> fields{
            {.name = "authorization", .value = "~", .never_indexed = true}
        };
        enc.encode(fields, out);
        CHECK(to_hex(out) == "1f08017e"); // index 23, with 4-bit prefix: 15, then 8
    }

    SECTION("a never-indexed field stays a never-indexed literal, even if the static table has it")
    {
        // found by fuzzing: as an index, it would no longer say it's never to be indexed.
        const std::vector<hpack::field> fields{
            {.name = ":authority", .value = "", .never_indexed = true}
        };
        enc.encode(fields, out);
        CHECK(to_hex(out) == "1100");
    }

    SECTION("Huffman coding, when it's shorter")
    {
        const std::vector<hpack::field> fields{
            {.name = "custom-key", .value = "custom-value"}
        };
        enc.encode(fields, out);
        CHECK(to_hex(out) == "008825a849e95ba97d7f8925a849e95bb8e8b4bf");
    }
}

TEST_CASE("the encoder signals the table's size, once, without indexing too", "[http][hpack][encoder]")
{
    hpack::encoder enc;

    // NOTE: without indexing the table stays empty, but its size follows the peer's all the same, growing too: the
    //       smallest since the last block, then the final size.
    enc.set_max_table_size(8'192);
    enc.set_max_table_size(100);
    enc.set_max_table_size(200);
    CHECK(enc.max_table_size() == 200);

    const std::vector<hpack::field> fields{
        {.name = ":method", .value = "GET"}
    };
    std::vector<std::byte> first;
    enc.encode(fields, first);
    CHECK(to_hex(first) == "3f453fa90182");

    std::vector<std::byte> second;
    enc.encode(fields, second);
    CHECK(to_hex(second) == "82");

    // a decoder whose limit is lowered to 200 accepts it.
    hpack::decoder dec;
    dec.set_max_table_size(200);
    CHECK_FALSE(decode(dec, first).err);
    CHECK(dec.table().max_size() == 200);
    CHECK(enc.table().count() == 0);
}

TEST_CASE("credentials and cookies are sensitive", "[http][hpack]")
{
    CHECK(hpack::sensitive("authorization"));
    CHECK(hpack::sensitive("proxy-authorization"));
    CHECK(hpack::sensitive("cookie"));
    CHECK(hpack::sensitive("set-cookie"));
    CHECK_FALSE(hpack::sensitive("content-type"));
}

namespace
{

// model is a dynamic table as RFC 7541 describes it, newest first, to check dynamic_table's storage against.
struct model
{
    std::deque<std::pair<std::string, std::string>> fields;
    std::size_t                                     size = 0;
    std::size_t                                     max  = hpack::default_table_size;

    void evict_to(std::size_t to)
    {
        while (size > to)
        {
            size -= hpack::field_size(fields.back().first, fields.back().second);
            fields.pop_back();
        }
    }

    void insert(std::string name, std::string value)
    {
        const auto added = hpack::field_size(name, value);
        if (added > max)
        {
            fields.clear();
            size = 0;
            return;
        }

        evict_to(max - added);
        fields.emplace_front(std::move(name), std::move(value));
        size += added;
    }

    void set_max(std::size_t to)
    {
        max = to;
        evict_to(max);
    }

    [[nodiscard]] field_list table() const { return {fields.begin(), fields.end()}; }
};

// encode_with is enc's block of fields.
std::vector<std::byte> encode_with(hpack::encoder& enc, const field_list& fields)
{
    std::vector<hpack::field> list;
    for (const auto& [name, value] : fields) list.push_back({.name = name, .value = value});

    std::vector<std::byte> out;
    enc.encode(list, out);
    return out;
}

}

TEST_CASE("with indexing, the RFC's requests encode as its Huffman-coded examples do",
          "[http][hpack][encoder][indexing]")
{
    // C.4 (C.3's requests, Huffman coded): :authority and cache-control indexed, the rest from the static table.
    const auto seq = rfc_examples()[5];
    REQUIRE(seq.name == "C.4: requests with Huffman coding");

    hpack::encoder enc{hpack::indexing::automatic};
    hpack::decoder dec;
    for (const auto& ex : seq.examples)
    {
        CAPTURE(ex.encoded);

        const auto block = encode_with(enc, ex.fields);
        CHECK(to_hex(block) == ex.encoded);
        CHECK(table_of(enc.table()) == ex.table);
        CHECK(enc.table().size() == ex.table_size);

        const auto got = decode(dec, block);
        CHECK_FALSE(got.err);
        CHECK(got.fields == owned(ex.fields));
        CHECK(table_of(dec.table()) == ex.table);
    }
}

TEST_CASE("with indexing, the RFC's responses decode to the same fields, leaving the tables the policy says",
          "[http][hpack][encoder][indexing]")
{
    // C.6's responses, with a table of 256: location's a literal without indexing, and set-cookie never indexed, so
    // the tables differ from the RFC's, whose encoder indexes both.
    const auto seq = rfc_examples()[7];
    REQUIRE(seq.table_size == 256);

    const std::vector<std::pair<field_list, std::size_t>> tables{
        {{{"date", "Mon, 21 Oct 2013 20:13:21 GMT"}, {"cache-control", "private"}, {":status", "302"}},159        },
        {                                                                {{":status", "307"},
                                                                {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
                                                                {"cache-control", "private"},
                                                                {":status", "302"}},
         201},
        {                                         {{"content-encoding", "gzip"},
                                         {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
                                         {":status", "307"},
                                         {"date", "Mon, 21 Oct 2013 20:13:21 GMT"}},
         224},
    };

    hpack::encoder enc{hpack::indexing::automatic};
    enc.set_max_table_size(seq.table_size);
    hpack::decoder dec;
    dec.set_max_table_size(seq.table_size); // acknowledged: the first block must start with a size update

    for (std::size_t i = 0; i < seq.examples.size(); ++i)
    {
        const auto& ex = seq.examples[i];
        CAPTURE(i);

        const auto block = encode_with(enc, ex.fields);
        if (i == 0) CHECK(to_hex(block).starts_with("3fe101")); // 256

        const auto got = decode(dec, block);
        REQUIRE_FALSE(got.err);
        CHECK(got.fields == owned(ex.fields));
        for (std::size_t j = 0; j < got.fields.size(); ++j)
            CHECK(got.never_indexed[j] == (got.fields[j].first == "set-cookie"));

        CHECK(table_of(enc.table()) == tables[i].first);
        CHECK(enc.table().size() == tables[i].second);
        CHECK(table_of(dec.table()) == tables[i].first);
    }
}

TEST_CASE("the encoder signals the smallest size since the last block, then the final one",
          "[http][hpack][encoder][size]")
{
    struct test_case
    {
        std::string_view         what;
        hpack::indexing          mode = hpack::indexing::automatic;
        std::vector<std::size_t> sizes;
        std::string_view         hex; // the block of ":method: GET"
    };

    const auto tc = GENERATE(
        test_case{
            .what  = "shrinking",
            .sizes = {100},
            .hex   = "3f4582"
    },
        test_case{.what = "growing", .sizes = {8'192}, .hex = "3fe13f82"},
        test_case{.what = "shrinking, then growing back", .sizes = {0, 4'096}, .hex = "203fe11f82"},
        test_case{.what = "shrinking, then growing past", .sizes = {100, 8'192}, .hex = "3f453fe13f82"},
        test_case{.what = "growing, then shrinking back", .sizes = {8'192, 4'096}, .hex = "82"},
        test_case{.what = "growing, then shrinking below", .sizes = {8'192, 100}, .hex = "3f4582"},
        test_case{.what = "shrinking twice", .sizes = {200, 100}, .hex = "3f4582"},
        test_case{.what = "the same", .sizes = {4'096}, .hex = "82"},
        test_case{.what  = "without indexing, growing",
                  .mode  = hpack::indexing::none,
                  .sizes = {8'192},
                  .hex   = "3fe13f82"},
        test_case{.what  = "without indexing, shrinking, then growing",
                  .mode  = hpack::indexing::none,
                  .sizes = {0, 4'096},
                  .hex   = "203fe11f82"});
    CAPTURE(tc.what);

    hpack::encoder enc{tc.mode};
    hpack::decoder dec{hpack::default_table_size};
    for (const auto size : tc.sizes)
    {
        enc.set_max_table_size(size);
        dec.set_max_table_size(size); // each acknowledged
    }

    const field_list fields{
        {":method", "GET"}
    };
    const auto first = encode_with(enc, fields);
    CHECK(to_hex(first) == tc.hex);
    CHECK(to_hex(encode_with(enc, fields)) == "82"); // signalled once

    const auto got = decode(dec, first);
    CHECK_FALSE(got.err);
    CHECK(dec.table().max_size() == enc.max_table_size());
}

TEST_CASE("a table shrunk and grown again between blocks is emptied on both sides", "[http][hpack][encoder][size]")
{
    hpack::encoder enc{hpack::indexing::automatic};
    hpack::decoder dec;

    const field_list fields{
        {":authority", "example.com"}
    };
    REQUIRE_FALSE(decode(dec, encode_with(enc, fields)).err);
    REQUIRE(enc.table().count() == 1);
    REQUIRE(to_hex(encode_with(enc, fields)) == "be"); // indexed now

    enc.set_max_table_size(0);
    enc.set_max_table_size(4'096);
    const auto block = encode_with(enc, fields);
    CHECK(to_hex(block).starts_with("203fe11f41")); // 0, 4096, then a literal with indexing again
    REQUIRE_FALSE(decode(dec, block).err);
    CHECK(table_of(enc.table()) == table_of(dec.table()));
    CHECK(enc.table().count() == 1);
}

TEST_CASE("the encoder's table evicts the oldest fields, as the decoder's does", "[http][hpack][encoder][indexing]")
{
    hpack::encoder enc{hpack::indexing::automatic};
    hpack::decoder dec;

    // each 1035 bytes (by field_size): three fit in 4096.
    const std::string value(1'000, 'v');
    for (const auto name : {"x-a"sv, "x-b"sv, "x-c"sv, "x-d"sv})
    {
        const field_list fields{
            {name, value}
        };
        REQUIRE_FALSE(decode(dec, encode_with(enc, fields)).err);
        CHECK(table_of(enc.table()) == table_of(dec.table()));
    }

    CHECK(table_of(enc.table())
          == field_list{
              {"x-d", value},
              {"x-c", value},
              {"x-b", value}
    });
    CHECK(enc.table().size() == std::size_t{3} * 1'035);

    // x-a is gone, so it's added again (evicting x-b); x-c is still there, so it's an index.
    const field_list again{
        {"x-a", value},
        {"x-c", value}
    };
    const auto block = encode_with(enc, again);
    CHECK(to_hex(block).ends_with("c0")); // index 64: x-c, behind x-a and x-d
    REQUIRE_FALSE(decode(dec, block).err);
    CHECK(table_of(enc.table())
          == field_list{
              {"x-a", value},
              {"x-d", value},
              {"x-c", value}
    });
    CHECK(table_of(dec.table()) == table_of(enc.table()));
}

TEST_CASE("never-indexed fields, and those that change per message, stay out of the encoder's table",
          "[http][hpack][encoder][indexing]")
{
    const std::vector<hpack::field> fields{
        {.name = "authorization", .value = "Bearer secret"},
        {.name = "proxy-authorization", .value = "Basic c2VjcmV0"},
        {.name = "cookie", .value = "a=b"},
        {.name = "set-cookie", .value = "a=b; Secure"},
        {.name = "x-secret", .value = "c", .never_indexed = true},
        {.name = ":path", .value = "/a"},
        {.name = "age", .value = "1"},
        {.name = "content-length", .value = "10"},
        {.name = "etag", .value = R"("x")"},
        {.name = "if-modified-since", .value = "Mon, 21 Oct 2013 20:13:21 GMT"},
        {.name = "if-none-match", .value = R"("x")"},
        {.name = "location", .value = "/b"},
    };

    hpack::encoder         enc{hpack::indexing::automatic};
    std::vector<std::byte> first;
    enc.encode(fields, first);
    CHECK(enc.table().count() == 0);

    // the same again: no indexes, as nothing was added.
    std::vector<std::byte> second;
    enc.encode(fields, second);
    CHECK(second == first);

    // sensitive() names are sent as never indexed, whatever the field said.
    hpack::decoder dec;
    const auto     got = decode(dec, first);
    REQUIRE_FALSE(got.err);
    CHECK(got.never_indexed
          == std::vector<bool>{true, true, true, true, true, false, false, false, false, false, false, false});
    CHECK(dec.table().count() == 0);
}

TEST_CASE("a field the encoder's table has is a never-indexed literal when it's marked so",
          "[http][hpack][encoder][indexing]")
{
    hpack::encoder enc{hpack::indexing::automatic};
    hpack::decoder dec;

    const std::vector<hpack::field> indexed{
        {.name = "x-token", .value = "abc"}
    };
    std::vector<std::byte> block;
    enc.encode(indexed, block);
    REQUIRE_FALSE(decode(dec, block).err);
    REQUIRE(enc.table().count() == 1);

    const std::vector<hpack::field> never{
        {.name = "x-token", .value = "abc", .never_indexed = true}
    };
    block.clear();
    enc.encode(never, block);
    CHECK(to_hex(block).starts_with("1f2f")); // never indexed (0001), its name index 62 (15, then 47)

    const auto got = decode(dec, block);
    REQUIRE_FALSE(got.err);
    CHECK(got.never_indexed == std::vector<bool>{true});
    CHECK(enc.table().count() == 1);
    CHECK(dec.table().count() == 1);
}

TEST_CASE("the encoder indexes a field of at most 3/4 of its table", "[http][hpack][encoder][indexing]")
{
    struct test_case
    {
        std::size_t table_size;
        std::size_t value_size; // of a field named "x": 33 more by field_size
        bool        indexed;
    };

    const auto tc = GENERATE(test_case{.table_size = 4'096, .value_size = 3'039, .indexed = true},
                             test_case{.table_size = 4'096, .value_size = 3'040, .indexed = false},
                             test_case{.table_size = 100, .value_size = 42, .indexed = true},
                             test_case{.table_size = 100, .value_size = 43, .indexed = false},
                             test_case{.table_size = 0, .value_size = 0, .indexed = false});
    CAPTURE(tc.table_size, tc.value_size);

    hpack::encoder enc{hpack::indexing::automatic};
    enc.set_max_table_size(tc.table_size);

    const std::string value(tc.value_size, 'v');
    const field_list  fields{
        {"x", value}
    };
    encode_with(enc, fields);
    CHECK(enc.table().count() == (tc.indexed ? 1U : 0U));
}

TEST_CASE("the dynamic table's storage keeps its fields as they move to its front and it grows", "[http][hpack][table]")
{
    // a fixed sequence of inserts (of new fields, and of fields named or valued from the table's own) and size changes,
    // checked against the model after each.
    // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp): the same sequence each run.
    std::minstd_rand random{42};
    const auto below = [&](std::size_t n) { return std::uniform_int_distribution<std::size_t>{0, n - 1}(random); };
    hpack::dynamic_table table;
    model                expected;

    for (int i = 0; i < 20'000; ++i)
    {
        CAPTURE(i);

        const auto what = below(100);
        if (what < 2)
        {
            const auto size = below(6'000);
            table.set_max_size(size);
            expected.set_max(size);
        }
        else
        {
            std::string      name(below(40) + 1, static_cast<char>('a' + below(26)));
            std::string      value(below(what < 10 ? 2'000 : 200), static_cast<char>('A' + below(26)));
            std::string_view name_view  = name;
            std::string_view value_view = value;
            if (table.count() > 0 && what < 40) name_view = table.at(below(table.count())).name;
            if (table.count() > 0 && what < 20) value_view = table.at(below(table.count())).value;

            expected.insert(std::string{name_view}, std::string{value_view});
            table.insert(name_view, value_view);
        }

        REQUIRE(table.size() == expected.size);
        REQUIRE(table_of(table) == expected.table());

        if (table.count() > 0)
        {
            // find() gives the newest with the name and value.
            const auto index = below(table.count());
            const auto f     = table.at(index);
            const auto found = table.find(f.name, f.value);
            REQUIRE(found.index <= index);
            REQUIRE(table.at(found.index) == hpack::field{.name = f.name, .value = f.value});
            REQUIRE(found.name_index <= found.index);
            REQUIRE(table.at(found.name_index).name == f.name);
        }
    }

    CHECK(table.find("not", "there").index == hpack::dynamic_table::npos);
}

TEST_CASE("a field named or valued from one it evicts is copied as the storage moves to its front",
          "[http][hpack][table]")
{
    // with a maximum size of 100, the storage is 200 bytes: y's text (68 bytes) is at 66, and z's (68 more) doesn't fit
    // after it, so y's is moved to the front, over where it was, before z is copied from it.
    std::string y_value;
    for (int i = 0; i < 67; ++i) y_value.push_back(static_cast<char>('0' + (i % 43)));

    hpack::dynamic_table table{100};
    table.insert("x", std::string(65, 'x')); // 98
    table.insert("n", y_value);              // 100: evicts x
    REQUIRE(table_of(table)
            == field_list{
                {"n", y_value}
    });

    SECTION("named")
    {
        const std::string z_value(67, 'z');
        table.insert(table.at(0).name, z_value);
        CHECK(table_of(table)
              == field_list{
                  {"n", z_value}
        });
    }

    SECTION("valued")
    {
        table.insert("q", table.at(0).value);
        CHECK(table_of(table)
              == field_list{
                  {"q", y_value}
        });
    }
}

TEST_CASE("a field named from one in the table is copied as the storage grows", "[http][hpack][table]")
{
    // the storage starts at 512 bytes: the second field doesn't fit after the first (whose name it has), and with it
    // they'd be more than half of it, so both go to a larger one.
    hpack::dynamic_table table;
    table.insert("x-first", std::string(100, 'a'));

    const std::string value(450, 'b');
    table.insert(table.at(0).name, value);
    CHECK(table_of(table)
          == field_list{
              {"x-first", value},
              {"x-first", std::string(100, 'a')}
    });
}

TEST_CASE("decoding's fields stay valid through their callbacks as the table's storage moves",
          "[http][hpack][decoder][table]")
{
    // literals with indexing named after the newest field (index 62, a view of the table's storage as it's copied
    // into the table again), with values long enough that the storage moves to its front every few fields.
    hpack::decoder dec{256};
    model          expected;
    expected.set_max(256);
    expected.insert("x-name", "first");

    std::vector<std::byte> start{std::byte{0x40}, std::byte{6}};
    for (const auto c : "x-name"sv) start.push_back(static_cast<std::byte>(c));
    start.push_back(std::byte{5});
    for (const auto c : "first"sv) start.push_back(static_cast<std::byte>(c));
    REQUIRE_FALSE(decode(dec, start).err);

    for (int i = 0; i < 1'000; ++i)
    {
        CAPTURE(i);

        std::vector<std::byte>   block;
        std::vector<std::string> values;
        for (int j = 0; j < 3; ++j)
        {
            values.emplace_back(static_cast<std::size_t>(20 + (((i * 7) + (j * 13)) % 60)), static_cast<char>('a' + j));
            hpack::encode_integer(62, 6, 0x40, block);
            hpack::encode_integer(static_cast<std::uint32_t>(values.back().size()), 7, 0, block);
            for (const auto c : values.back()) block.push_back(static_cast<std::byte>(c));
        }
        block.push_back(std::byte{0xbe}); // and the newest, indexed

        std::vector<std::pair<std::string, std::string>> want;
        for (const auto& v : values)
        {
            want.emplace_back("x-name", v);
            expected.insert("x-name", v);
        }
        want.emplace_back("x-name", values.back());

        // the callback compares what it's given while it's given it.
        std::size_t seen = 0;
        bool        same = true;
        const auto  err  = dec.decode(block,
                                      [&](const hpack::field& f)
                                      {
                                        same = same && seen < want.size() && f.name == want[seen].first
                                            && f.value == want[seen].second;
                                        ++seen;
                                      });
        REQUIRE_FALSE(err);
        CHECK(same);
        CHECK(seen == want.size());
        REQUIRE(table_of(dec.table()) == expected.table());
    }
}
