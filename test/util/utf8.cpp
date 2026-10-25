#include "util/utf8.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include "util/utf8_reference.hpp"

using namespace std::string_view_literals;

namespace
{

std::span<const std::byte> bytes_of(std::string_view s) noexcept { return std::as_bytes(std::span{s}); }

// byte_at_a_time feeds bytes one at a time, returning where it failed (bytes' size if it didn't) and whether it ended
// complete.
test::utf8_result byte_at_a_time(std::span<const std::byte> bytes)
{
    net::util::utf8_validator utf8;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (!utf8.feed(bytes[i])) return {.bad = i};
    }
    return {.bad = bytes.size(), .complete = utf8.complete()};
}

// whole feeds bytes at once: whether it was accepted, and complete.
test::utf8_result whole(std::span<const std::byte> bytes)
{
    net::util::utf8_validator utf8;
    if (!utf8.feed(bytes)) return {.bad = 0};
    return {.bad = bytes.size(), .complete = utf8.complete()};
}

}

TEST_CASE("utf8_validator accepts UTF-8", "[util][utf8]")
{
    const auto text = GENERATE(as<std::string_view>{},
                               "",
                               "plain ASCII, more than a word of it",
                               "\x00\x7f"sv,
                               "\xc2\x80",         // U+0080, the first 2-byte form
                               "\xdf\xbf",         // U+07FF
                               "\xe0\xa0\x80",     // U+0800, the first 3-byte form
                               "\xed\x9f\xbf",     // U+D7FF, before the surrogates
                               "\xee\x80\x80",     // U+E000, after them
                               "\xef\xbf\xbf",     // U+FFFF
                               "\xf0\x90\x80\x80", // U+10000, the first 4-byte form
                               "\xf4\x8f\xbf\xbf", // U+10FFFF, the last
                               "Kalimera, \xce\xba\xe1\xbd\xb9\xcf\x83\xce\xbc\xce\xb5 \xf0\x9f\x98\x80!");
    CAPTURE(text);

    CHECK(byte_at_a_time(bytes_of(text)) == test::utf8_result{.bad = text.size(), .complete = true});
    CHECK(whole(bytes_of(text)) == test::utf8_result{.bad = text.size(), .complete = true});
}

TEST_CASE("utf8_validator refuses what isn't UTF-8, at the first byte that can't be", "[util][utf8]")
{
    struct test_case
    {
        std::string_view text;
        std::size_t      bad; // the byte it fails at
    };
    const auto tc = GENERATE(test_case{.text = "\x80", .bad = 0},                     // a continuation byte alone
                             test_case{.text = "ab\xbf", .bad = 2},                   // the same, after ASCII
                             test_case{.text = "\xc0\x80", .bad = 0},                 // overlong NUL
                             test_case{.text = "\xc1\xbf", .bad = 0},                 // overlong U+007F
                             test_case{.text = "\xe0\x80\x80", .bad = 1},             // overlong 3-byte NUL
                             test_case{.text = "\xe0\x9f\xbf", .bad = 1},             // overlong U+07FF
                             test_case{.text = "\xf0\x80\x80\x80", .bad = 1},         // overlong 4-byte NUL
                             test_case{.text = "\xf0\x8f\xbf\xbf", .bad = 1},         // overlong U+FFFF
                             test_case{.text = "\xed\xa0\x80", .bad = 1},             // U+D800, a surrogate
                             test_case{.text = "\xed\xbf\xbf", .bad = 1},             // U+DFFF
                             test_case{.text = "\xed\xa0\x80\xed\xb0\x80", .bad = 1}, // a surrogate pair (CESU-8)
                             test_case{.text = "\xf4\x90\x80\x80", .bad = 1},         // U+110000
                             test_case{.text = "\xf5\x80\x80\x80", .bad = 0},         // past U+10FFFF by its lead
                             test_case{.text = "\xf7\xbf\xbf\xbf", .bad = 0},
                             test_case{.text = "\xf8\x88\x80\x80\x80", .bad = 0},     // a 5-byte form
                             test_case{.text = "\xfc\x84\x80\x80\x80\x80", .bad = 0}, // a 6-byte form
                             test_case{.text = "\xfe", .bad = 0},
                             test_case{.text = "\xff", .bad = 0},
                             test_case{.text = "\xc3\x41", .bad = 1},              // cut short by ASCII
                             test_case{.text = "\xe2\x82\xc3\xa9", .bad = 2},      // cut short by another's lead
                             test_case{.text = "\xf0\x9f\x98\x80\x80", .bad = 4}); // a continuation too many
    CAPTURE(tc.text);

    CHECK(byte_at_a_time(bytes_of(tc.text)).bad == tc.bad);
    CHECK(test::utf8_reference(bytes_of(tc.text)).bad == tc.bad);

    net::util::utf8_validator utf8;
    CHECK_FALSE(utf8.feed(bytes_of(tc.text)));
}

TEST_CASE("utf8_validator says a character cut short at the end isn't complete", "[util][utf8][complete]")
{
    const auto text =
        GENERATE(as<std::string_view>{}, "\xc3", "\xe2\x82", "\xf0\x9f\x98", "abcdefghij\xf0", "\xc3\xa9\xe2");
    CAPTURE(text);

    net::util::utf8_validator utf8;
    CHECK(utf8.feed(bytes_of(text)));
    CHECK_FALSE(utf8.complete());

    utf8.reset();
    CHECK(utf8.complete());
}

TEST_CASE("utf8_validator takes a character split across feeds at any byte", "[util][utf8][split]")
{
    // each multi-byte form, between ASCII, so the word-at-a-time path runs before and after the split
    const auto text = GENERATE(as<std::string_view>{},
                               "abcdefghij\xc3\xa9klmnopqrst",
                               "abcdefghij\xe2\x82\xacklmnopqrst",
                               "abcdefghij\xf0\x9f\x98\x80klmnopqrst",
                               "\xf4\x8f\xbf\xbf\xe0\xa0\x80\xc2\x80\xed\x9f\xbf");
    const auto at   = GENERATE_COPY(range(std::size_t{0}, text.size() + 1));
    CAPTURE(text, at);

    const auto bytes = bytes_of(text);

    net::util::utf8_validator utf8;
    REQUIRE(utf8.feed(bytes.first(at)));
    CHECK(utf8.complete() == (test::utf8_reference(bytes.first(at)).complete));
    REQUIRE(utf8.feed(bytes.subspan(at)));
    CHECK(utf8.complete());
}

TEST_CASE("utf8_validator finds a bad byte anywhere in a run of ASCII", "[util][utf8][ascii]")
{
    // 40 bytes of ASCII, from each of a word's offsets in the buffer, with something at each offset: the word-at-a-time
    // path has to stop for it, wherever it falls in a word.
    const auto offset = GENERATE(range(std::size_t{0}, std::size_t{8}));
    const auto at     = GENERATE(range(std::size_t{0}, std::size_t{40}));
    CAPTURE(offset, at);

    std::array<std::byte, 48> buffer{};
    const auto                run = std::span{buffer}.subspan(offset, 40);
    for (auto& b : run) b = std::byte{'a'};

    SECTION("a byte that can't be anywhere")
    {
        const auto bad = GENERATE(std::byte{0x80}, std::byte{0xbf}, std::byte{0xc0}, std::byte{0xf8}, std::byte{0xff});
        run[at]        = bad;
        net::util::utf8_validator utf8;
        CHECK_FALSE(utf8.feed(run));
    }

    SECTION("a lead byte cut short by the ASCII after it, or by the end")
    {
        run[at] = std::byte{0xc3};
        net::util::utf8_validator utf8;
        if (at + 1 < run.size())
        {
            CHECK_FALSE(utf8.feed(run));
        }
        else
        {
            CHECK(utf8.feed(run));
            CHECK_FALSE(utf8.complete());
        }
    }

    SECTION("a character, which the run goes on after")
    {
        if (at + 2 > run.size()) return;
        run[at]     = std::byte{0xc3};
        run[at + 1] = std::byte{0xa9};
        net::util::utf8_validator utf8;
        CHECK(utf8.feed(run));
        CHECK(utf8.complete());
    }
}

TEST_CASE("utf8_validator agrees with a decoder of code points", "[util][utf8][reference]")
{
    // every sequence of one and two bytes, and of three and four from every lead byte with continuations at the
    // ranges' edges; fed a byte at a time (failing at the same byte) and at once.
    constexpr std::array<std::uint8_t, 12>
        edges{0x00, 0x41, 0x7f, 0x80, 0x8f, 0x90, 0x9f, 0xa0, 0xbf, 0xc0, 0xc3, 0xff};

    std::size_t                         checked = 0;
    std::vector<std::vector<std::byte>> mismatches;
    const auto                          check = [&](std::span<const std::byte> bytes)
    {
        ++checked;
        const auto expected  = test::utf8_reference(bytes);
        const bool valid     = expected.bad == bytes.size();
        const auto got_whole = whole(bytes);
        const bool agree     = byte_at_a_time(bytes) == expected && (got_whole.bad == bytes.size()) == valid
                            && (!valid || got_whole.complete == expected.complete);
        if (!agree && mismatches.size() < 10) mismatches.emplace_back(bytes.begin(), bytes.end());
    };

    std::array<std::byte, 4> seq{};
    for (unsigned a = 0; a < 256; ++a)
    {
        seq[0] = std::byte(a);
        check(std::span{seq}.first(1));
        for (unsigned b = 0; b < 256; ++b)
        {
            seq[1] = std::byte(b);
            check(std::span{seq}.first(2));
        }
        for (auto b : edges)
        {
            seq[1] = std::byte{b};
            for (auto c : edges)
            {
                seq[2] = std::byte{c};
                check(std::span{seq}.first(3));
                for (auto d : edges)
                {
                    seq[3] = std::byte{d};
                    check(std::span{seq}.first(4));
                }
            }
        }
    }

    CAPTURE(checked);
    for (const auto& m : mismatches)
    {
        std::string hex;
        for (auto b : m) hex += std::format("{:02x} ", static_cast<unsigned>(b));
        UNSCOPED_INFO(hex);
    }
    CHECK(mismatches.empty());
}
