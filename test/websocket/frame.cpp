#include "websocket/frame.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include "websocket/error.hpp"
#include "websocket/frames.hpp"

namespace
{

using namespace std::string_view_literals;

using net::websocket::close_code;
using test::websocket::bytes;
using test::websocket::unhex;

namespace ws = net::websocket::detail;

const auto protocol_error = net::websocket::sent(close_code::protocol_error);

// RFC 6455 5.7's masking key.
constexpr std::array<std::byte, 4> example_key{std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}};

// decoded is in's header, which must decode whole.
ws::decoded_header decoded(std::span<const std::byte> in)
{
    const auto out = ws::decode_header(in);
    REQUIRE(out.has_value());
    REQUIRE(out->has_value());
    return **out;
}

std::vector<std::byte> encoded(const ws::frame_header& header)
{
    std::array<std::byte, ws::max_header_size> out{};
    const auto                                 size = ws::encode_header(header, out);
    return {out.begin(), out.begin() + static_cast<std::ptrdiff_t>(size)};
}

}

TEST_CASE("frame headers decode RFC 6455's examples", "[websocket][frame]")
{
    struct test_case
    {
        std::string_view name;
        std::string_view hex; // the frame's header, and its payload if it's short
        ws::frame_header header;
        std::string_view payload{}; // unmasked
    };

    auto tc = GENERATE(
        test_case{
            .name    = "a single-frame unmasked text message",
            .hex     = "81 05 48 65 6c 6c 6f",
            .header  = {.fin = true, .op = ws::opcode::text, .length = 5},
            .payload = "Hello"
    },
        test_case{.name   = "a single-frame masked text message",
                  .hex    = "81 85 37 fa 21 3d 7f 9f 4d 51 58",
                  .header = {.fin = true, .op = ws::opcode::text, .masked = true, .mask_key = example_key, .length = 5},
                  .payload = "Hello"},
        test_case{.name    = "a fragmented unmasked text message's first frame",
                  .hex     = "01 03 48 65 6c",
                  .header  = {.fin = false, .op = ws::opcode::text, .length = 3},
                  .payload = "Hel"},
        test_case{.name    = "a fragmented unmasked text message's last frame",
                  .hex     = "80 02 6c 6f",
                  .header  = {.fin = true, .op = ws::opcode::continuation, .length = 2},
                  .payload = "lo"},
        test_case{.name    = "an unmasked ping",
                  .hex     = "89 05 48 65 6c 6c 6f",
                  .header  = {.fin = true, .op = ws::opcode::ping, .length = 5},
                  .payload = "Hello"},
        test_case{.name   = "a masked pong",
                  .hex    = "8a 85 37 fa 21 3d 7f 9f 4d 51 58",
                  .header = {.fin = true, .op = ws::opcode::pong, .masked = true, .mask_key = example_key, .length = 5},
                  .payload = "Hello"},
        test_case{.name   = "256 bytes of binary in a single unmasked frame",
                  .hex    = "82 7e 01 00",
                  .header = {.fin = true, .op = ws::opcode::binary, .length = 256}},
        test_case{.name   = "64 KiB of binary in a single unmasked frame",
                  .hex    = "82 7f 00 00 00 00 00 01 00 00",
                  .header = {.fin = true, .op = ws::opcode::binary, .length = 65536}});

    CAPTURE(tc.name);

    const auto in  = unhex(tc.hex);
    const auto out = decoded(in);
    CHECK(out.header == tc.header);
    CHECK(out.size == ws::header_size(tc.header));
    CHECK(encoded(tc.header) == std::vector<std::byte>(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(out.size)));

    if (!tc.payload.empty())
    {
        std::vector<std::byte> payload(in.begin() + static_cast<std::ptrdiff_t>(out.size), in.end());
        if (out.header.masked) ws::apply_mask(payload, out.header.mask_key, 0);
        CHECK(payload == bytes(tc.payload));
    }
}

TEST_CASE("frame headers encode and decode every length form", "[websocket][frame]")
{
    const auto length = GENERATE(std::uint64_t{0},
                                 std::uint64_t{1},
                                 std::uint64_t{125},
                                 std::uint64_t{126},
                                 std::uint64_t{127},
                                 std::uint64_t{65535},
                                 std::uint64_t{65536},
                                 std::uint64_t{1} << 32,
                                 (std::uint64_t{1} << 63) - 1);
    const auto masked = GENERATE(false, true);
    const auto op     = GENERATE(ws::opcode::continuation, ws::opcode::binary, ws::opcode::close, ws::opcode{0xf});
    const auto rsv    = GENERATE(std::uint8_t{0}, std::uint8_t{5});

    CAPTURE(length, masked, op, rsv);

    const ws::frame_header header{.fin      = op != ws::opcode::continuation,
                                  .rsv      = rsv,
                                  .op       = op,
                                  .masked   = masked,
                                  .mask_key = example_key,
                                  .length   = length};
    const auto             in = encoded(header);

    std::size_t extended = 0;
    if (length > 0xffff) extended = 8;
    else if (length >= 126) extended = 2;
    CHECK(in.size() == 2 + extended + (masked ? 4 : 0));
    CHECK(in.size() == ws::header_size(header));

    const auto out = decoded(in);
    CHECK(out.header
          == (masked ? header : ws::frame_header{.fin = header.fin, .rsv = rsv, .op = op, .length = length}));
    CHECK(out.size == in.size());

    // every prefix is too short: decoding asks for more.
    for (std::size_t size = 0; size < in.size(); ++size)
    {
        CAPTURE(size);
        const auto partial = ws::decode_header(std::span{in}.first(size));
        REQUIRE(partial.has_value());
        CHECK_FALSE(partial->has_value());
    }
}

TEST_CASE("frame headers refuse lengths not in their minimal form, and the 64-bit length's top bit",
          "[websocket][frame]")
{
    struct test_case
    {
        std::string_view hex;
        bool             valid;
    };

    auto tc = GENERATE(test_case{.hex = "82 7e 00 00", .valid = false},
                       test_case{.hex = "82 7e 00 7d", .valid = false}, // 125
                       test_case{.hex = "82 7e 00 7e", .valid = true},  // 126
                       test_case{.hex = "82 7e ff ff", .valid = true},  // 65535
                       test_case{.hex = "82 7f 00 00 00 00 00 00 00 00", .valid = false},
                       test_case{.hex = "82 7f 00 00 00 00 00 00 00 7d", .valid = false},
                       test_case{.hex = "82 7f 00 00 00 00 00 00 ff ff", .valid = false}, // 65535
                       test_case{.hex = "82 7f 00 00 00 00 00 01 00 00", .valid = true},  // 65536
                       test_case{.hex = "82 7f 7f ff ff ff ff ff ff ff", .valid = true},  // 2^63 - 1
                       test_case{.hex = "82 7f 80 00 00 00 00 00 00 00", .valid = false}, // 2^63
                       test_case{.hex = "82 7f ff ff ff ff ff ff ff ff", .valid = false},
                       // masked: the length fails before the mask key's there.
                       test_case{.hex = "82 fe 00 7d", .valid = false},
                       test_case{.hex = "82 ff 00 00 00 00 00 00 ff ff", .valid = false});

    CAPTURE(tc.hex);

    const auto in = unhex(tc.hex);
    for (std::size_t size = 0; size < in.size(); ++size)
    {
        CAPTURE(size);
        const auto partial = ws::decode_header(std::span{in}.first(size));
        REQUIRE(partial.has_value());
        CHECK_FALSE(partial->has_value());
    }

    const auto out = ws::decode_header(in);
    if (tc.valid)
    {
        REQUIRE(out.has_value());
        CHECK(out->has_value());
    }
    else
    {
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error() == protocol_error);
    }
}

TEST_CASE("frame headers decode what's past the length check as it is", "[websocket][frame]")
{
    // reserved opcodes, RSV bits and the masks are the session's to judge.
    const auto out = decoded(unhex("7b 80 01 02 03 04 ff"));
    CHECK(out.size == 6);
    CHECK_FALSE(out.header.fin);
    CHECK(out.header.rsv == 7);
    CHECK(out.header.op == ws::opcode{0xb});
    CHECK(out.header.masked);
    CHECK(out.header.mask_key == std::array{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}});
    CHECK(out.header.length == 0);
}

TEST_CASE("masking is RFC 6455's, in any pieces", "[websocket][frame]")
{
    auto hello = bytes("Hello");
    ws::apply_mask(hello, example_key, 0);
    CHECK(hello == unhex("7f 9f 4d 51 58"));
    ws::apply_mask(hello, example_key, 0);
    CHECK(hello == bytes("Hello"));

    // a payload longer than a few words, at an address that isn't a word's.
    std::vector<std::byte> plain(1 + 61);
    for (std::size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<std::byte>((i * 7) + 3);
    const auto payload = std::span{plain}.subspan(1);

    std::vector<std::byte> expected(payload.begin(), payload.end());
    for (std::size_t i = 0; i < expected.size(); ++i) expected[i] ^= example_key[i % 4];

    SECTION("at once")
    {
        std::vector<std::byte> masked(payload.begin(), payload.end());
        ws::apply_mask(masked, example_key, 0);
        CHECK(masked == expected);
    }

    SECTION("in two pieces, split at every offset")
    {
        for (std::size_t split = 0; split <= payload.size(); ++split)
        {
            CAPTURE(split);
            std::vector<std::byte> copy(plain.begin(), plain.end());
            const auto             masked = std::span{copy}.subspan(1);
            ws::apply_mask(masked.first(split), example_key, 0);
            ws::apply_mask(masked.subspan(split), example_key, split);
            CHECK(std::vector<std::byte>(masked.begin(), masked.end()) == expected);
        }
    }

    SECTION("in pieces of every size")
    {
        for (std::size_t size = 1; size <= 17; ++size)
        {
            CAPTURE(size);
            std::vector<std::byte> masked(payload.begin(), payload.end());
            for (std::size_t at = 0; at < masked.size(); at += size)
                ws::apply_mask(std::span{masked}.subspan(at, std::min(size, masked.size() - at)), example_key, at);
            CHECK(masked == expected);
        }
    }

    SECTION("from an offset past 2^32")
    {
        const std::uint64_t    offset = (std::uint64_t{1} << 32) + 3;
        std::vector<std::byte> masked(payload.begin(), payload.end());
        ws::apply_mask(masked, example_key, offset);
        for (std::size_t i = 0; i < masked.size(); ++i)
        {
            CAPTURE(i);
            CHECK((masked[i] ^ payload[i]) == example_key[(offset + i) % 4]);
        }
    }
}

TEST_CASE("close payloads allow RFC 6455's codes, and refuse the rest", "[websocket][frame][close]")
{
    struct test_case
    {
        std::uint16_t code;
        bool          allowed;
    };

    auto tc = GENERATE(test_case{.code = 0, .allowed = false},
                       test_case{.code = 999, .allowed = false},
                       test_case{.code = 1000, .allowed = true},
                       test_case{.code = 1001, .allowed = true},
                       test_case{.code = 1002, .allowed = true},
                       test_case{.code = 1003, .allowed = true},
                       test_case{.code = 1004, .allowed = false},
                       test_case{.code = 1005, .allowed = false},
                       test_case{.code = 1006, .allowed = false},
                       test_case{.code = 1007, .allowed = true},
                       test_case{.code = 1008, .allowed = true},
                       test_case{.code = 1009, .allowed = true},
                       test_case{.code = 1010, .allowed = true},
                       test_case{.code = 1011, .allowed = true},
                       test_case{.code = 1012, .allowed = true},
                       test_case{.code = 1013, .allowed = true},
                       test_case{.code = 1014, .allowed = true},
                       test_case{.code = 1015, .allowed = false},
                       test_case{.code = 1016, .allowed = false},
                       test_case{.code = 1100, .allowed = false},
                       test_case{.code = 2000, .allowed = false},
                       test_case{.code = 2999, .allowed = false},
                       test_case{.code = 3000, .allowed = true},
                       test_case{.code = 3999, .allowed = true},
                       test_case{.code = 4000, .allowed = true},
                       test_case{.code = 4999, .allowed = true},
                       test_case{.code = 5000, .allowed = false},
                       test_case{.code = 65535, .allowed = false});

    CAPTURE(tc.code);

    const auto code = close_code{tc.code};
    CHECK(ws::sendable(code) == tc.allowed);

    const auto reason = GENERATE(""sv, "bye"sv);
    CAPTURE(reason);

    auto payload = std::vector<std::byte>{static_cast<std::byte>(tc.code >> 8), static_cast<std::byte>(tc.code & 0xff)};
    payload.insert(payload.end(), std::as_bytes(std::span{reason}).begin(), std::as_bytes(std::span{reason}).end());

    const auto                                     parsed = ws::parse_close(payload);
    std::array<std::byte, ws::max_control_payload> out{};
    const auto                                     size = ws::encode_close(code, reason, out);
    if (tc.allowed)
    {
        REQUIRE(parsed.has_value());
        CHECK(*parsed == ws::close_payload{.code = code, .reason = reason});

        REQUIRE(size.has_value());
        CHECK(std::vector<std::byte>(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(*size)) == payload);
    }
    else
    {
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == protocol_error);

        // 1005 alone is a close without a code (an empty payload): with a reason, it can't be sent either.
        if (code == close_code::no_status_received && reason.empty())
        {
            REQUIRE(size.has_value());
            CHECK(*size == 0);
        }
        else
        {
            REQUIRE_FALSE(size.has_value());
            CHECK(size.error() == std::errc::invalid_argument);
        }
    }
}

TEST_CASE("close payloads: none, 1 byte, and their reasons' UTF-8", "[websocket][frame][close]")
{
    SECTION("none is a close without a code")
    {
        const auto parsed = ws::parse_close({});
        REQUIRE(parsed.has_value());
        CHECK(parsed->code == close_code::no_status_received);
        CHECK(parsed->reason.empty());
    }

    SECTION("1 byte is malformed")
    {
        for (const auto b : {0x00, 0x03, 0xe8, 0xff})
        {
            CAPTURE(b);
            const auto parsed = ws::parse_close(std::vector{static_cast<std::byte>(b)});
            REQUIRE_FALSE(parsed.has_value());
            CHECK(parsed.error() == protocol_error);
        }
    }

    SECTION("reasons")
    {
        struct test_case
        {
            std::string_view name;
            std::string      reason;
            bool             valid;
        };

        auto tc = GENERATE(test_case{.name = "ASCII", .reason = "going home", .valid = true},
                           test_case{.name = "multi-byte", .reason = "été € \U0001f600", .valid = true},
                           test_case{.name = "the longest", .reason = std::string(123, 'r'), .valid = true},
                           test_case{.name = "a byte UTF-8 never has", .reason = "bad \xff", .valid = false},
                           test_case{.name = "a continuation byte alone", .reason = "\x80", .valid = false},
                           test_case{.name = "overlong", .reason = "\xc0\xaf", .valid = false},
                           test_case{.name = "a surrogate", .reason = "\xed\xa0\x80", .valid = false},
                           test_case{.name = "past U+10FFFF", .reason = "\xf4\x90\x80\x80", .valid = false},
                           test_case{.name = "cut short at the end", .reason = "euro \xe2\x82", .valid = false});

        CAPTURE(tc.name);

        auto       payload = test::websocket::join(unhex("03 e8"), bytes(tc.reason));
        const auto parsed  = ws::parse_close(payload);
        std::array<std::byte, ws::max_control_payload> out{};
        const auto encoded = ws::encode_close(close_code::normal_closure, tc.reason, out);
        if (tc.valid)
        {
            REQUIRE(parsed.has_value());
            CHECK(parsed->code == close_code::normal_closure);
            CHECK(parsed->reason == tc.reason);
            REQUIRE(encoded.has_value());
            CHECK(*encoded == payload.size());
        }
        else
        {
            REQUIRE_FALSE(parsed.has_value());
            CHECK(parsed.error() == protocol_error);
            REQUIRE_FALSE(encoded.has_value());
            CHECK(encoded.error() == std::errc::invalid_argument);
        }
    }

    SECTION("encode_close refuses a reason that doesn't fit")
    {
        std::array<std::byte, ws::max_control_payload> out{};
        const auto encoded = ws::encode_close(close_code::normal_closure, std::string(124, 'r'), out);
        REQUIRE_FALSE(encoded.has_value());
        CHECK(encoded.error() == std::errc::invalid_argument);
    }
}
