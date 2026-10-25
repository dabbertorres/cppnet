#include "util/sha1.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include "encoding/base64.hpp"
#include "encoding/hex.hpp"

namespace
{

// hex_digest is text's digest in hex (upper case, as hex::encode() writes it).
std::string hex_digest(std::string_view text)
{
    return net::encoding::hex::encode(net::util::sha1{}.update(text).finish());
}

// in_pieces hashes text fed size bytes at a time (the last piece what's left).
std::string in_pieces(std::string_view text, std::size_t size)
{
    net::util::sha1 hash;
    for (std::size_t at = 0; at < text.size(); at += size) hash.update(text.substr(at, size));
    return net::encoding::hex::encode(hash.finish());
}

// the 896-bit message of FIPS 180's examples (and RFC 3174's), spanning two blocks
constexpr std::string_view two_blocks =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnop"
    "qklmnopqrlmnopqrsmnopqrstnopqrstu";

}

TEST_CASE("sha1 gives FIPS 180's and RFC 3174's digests", "[util][sha1]")
{
    struct test_case
    {
        std::string      input;
        std::string_view digest;
    };
    const auto tc =
        GENERATE(test_case{.input = "", .digest = "DA39A3EE5E6B4B0D3255BFEF95601890AFD80709"},
                 test_case{.input = "abc", .digest = "A9993E364706816ABA3E25717850C26C9CD0D89D"},
                 test_case{.input  = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", // 448 bits
                           .digest = "84983E441C3BD26EBAAE4AA1F95129E5E54670F1"},
                 test_case{.input = std::string{two_blocks}, .digest = "A49B2446A02C645BF419F995B67091253A04A259"},
                 test_case{.input = std::string(1'000'000, 'a'), .digest = "34AA973CD4C4DAA4F61EEB2BDBAD27316534016F"},
                 test_case{.input =
                               []
                           {
                               std::string s;
                               for (int i = 0; i < 80; ++i) s += "01234567";
                               return s;
                           }(),
                           .digest = "DEA356A2CDDD90C7A7ECEDC5EBB563934F460452"}); // RFC 3174's fourth test
    CAPTURE(tc.input.size());

    CHECK(hex_digest(tc.input) == tc.digest);
}

TEST_CASE("sha1 pads lengths around a block's end", "[util][sha1][padding]")
{
    // 55 bytes leave room in their block for the padding's 0x80 and the length; 56 to 63 need a block more.
    struct test_case
    {
        std::size_t      length;
        std::string_view digest;
    };
    const auto tc = GENERATE(test_case{.length = 55, .digest = "C1C8BBDC22796E28C0E15163D20899B65621D65A"},
                             test_case{.length = 56, .digest = "C2DB330F6083854C99D4B5BFB6E8F29F201BE699"},
                             test_case{.length = 57, .digest = "F08F24908D682555111BE7FF6F004E78283D989A"},
                             test_case{.length = 63, .digest = "03F09F5B158A7A8CDAD920BDDC29B81C18A551F5"},
                             test_case{.length = 64, .digest = "0098BA824B5C16427BD7A1122A5A442A25EC644D"},
                             test_case{.length = 65, .digest = "11655326C708D70319BE2610E8A57D9A5B959D3B"},
                             test_case{.length = 119, .digest = "EE971065AAA017E0632A8CA6C77BB3BF8B1DFC56"},
                             test_case{.length = 120, .digest = "F34C1488385346A55709BA056DDD08280DD4C6D6"},
                             test_case{.length = 128, .digest = "AD5B3FDBCB526778C2839D2F151EA753995E26A0"});
    CAPTURE(tc.length);

    CHECK(hex_digest(std::string(tc.length, 'a')) == tc.digest);
}

TEST_CASE("sha1 gives the same digest however its input is split", "[util][sha1][split]")
{
    const std::string message = std::string{two_blocks} + std::string{two_blocks} + "and a little more"; // 241 bytes
    const auto        whole   = hex_digest(message);

    SECTION("in pieces of each size, across blocks' ends")
    {
        const auto size = GENERATE(range(std::size_t{1}, std::size_t{130}));
        CAPTURE(size);
        CHECK(in_pieces(message, size) == whole);
    }

    SECTION("in two pieces, split at each byte")
    {
        const auto at = GENERATE_COPY(range(std::size_t{0}, message.size() + 1));
        CAPTURE(at);
        net::util::sha1 hash;
        hash.update(std::string_view{message}.substr(0, at)).update(std::string_view{message}.substr(at));
        CHECK(net::encoding::hex::encode(hash.finish()) == whole);
    }

    SECTION("an empty piece changes nothing")
    {
        net::util::sha1 hash;
        hash.update("").update(message).update(std::span<const std::byte>{});
        CHECK(net::encoding::hex::encode(hash.finish()) == whole);
    }
}

TEST_CASE("sha1's finish leaves it as a new one", "[util][sha1]")
{
    net::util::sha1 hash;
    hash.update("some other input, which finish forgets");
    (void)hash.finish();

    CHECK(net::encoding::hex::encode(hash.update("abc").finish()) == "A9993E364706816ABA3E25717850C26C9CD0D89D");
}

TEST_CASE("sha1 makes RFC 6455's Sec-WebSocket-Accept example", "[util][sha1][websocket]")
{
    // RFC 6455 4.2.2 (and 1.3): base64 of SHA-1 of the client's key and the GUID.
    const auto digest =
        net::util::sha1{}.update("dGhlIHNhbXBsZSBub25jZQ==").update("258EAFA5-E914-47DA-95CA-C5AB0DC85B11").finish();

    CHECK(net::encoding::base64::encoding{}.encode(digest) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}
