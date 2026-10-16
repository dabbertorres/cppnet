#include "encoding/base64.hpp"

#include <array>
#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace
{

std::vector<std::byte> operator""_bytes(const char* str, std::size_t length)
{
    std::vector<std::byte> out;
    out.resize(length);

    for (auto i = 0u; i < length; ++i)
    {
        out[i] = static_cast<std::byte>(str[i]);
    }

    return out;
}

template<std::size_t N>
// NOLINTNEXTLINE(modernize-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
constexpr auto span_of(const char (&str)[N]) noexcept
{
    auto out = std::span<const char>{str};
    return out.subspan(0, out.size() - 1); // trim null terminator
}

}

using net::encoding::base64::encoding;

TEST_CASE("encoded_size", "[encoding][base64]")
{
    SECTION("with padding")
    {
        encoding enc;

        SECTION("under by 2")
        {
            CHECK(enc.encoded_size(1) == 4);
        }

        SECTION("under by 1")
        {
            CHECK(enc.encoded_size(2) == 4);
        }

        SECTION("multiple of 3")
        {
            CHECK(enc.encoded_size(3) == 4);
            CHECK(enc.encoded_size(6) == 8);
        }

        SECTION("over by 1")
        {
            CHECK(enc.encoded_size(4) == 8);
        }

        SECTION("over by 2")
        {
            CHECK(enc.encoded_size(5) == 8);
        }
    }

    SECTION("without padding")
    {
        encoding enc{net::encoding::base64::standard_alphabet(), 0};

        SECTION("under by 2")
        {
            CHECK(enc.encoded_size(1) == 2);
        }

        SECTION("under by 1")
        {
            CHECK(enc.encoded_size(2) == 3);
        }

        SECTION("multiple of 3")
        {
            CHECK(enc.encoded_size(3) == 4);
            CHECK(enc.encoded_size(6) == 8);
        }

        SECTION("over by 1")
        {
            CHECK(enc.encoded_size(4) == 6);
        }

        SECTION("over by 2")
        {
            CHECK(enc.encoded_size(5) == 7);
        }
    }
}

TEST_CASE("decoded_size", "[encoding][base64]")
{
    SECTION("with padding")
    {
        encoding enc;

        SECTION("under by 3")
        {
            CHECK(enc.decoded_size(1) == 0);
        }

        SECTION("under by 2")
        {
            CHECK(enc.decoded_size(2) == 0);
        }

        SECTION("under by 1")
        {
            CHECK(enc.decoded_size(3) == 0);
        }

        SECTION("multiple of 4")
        {
            CHECK(enc.decoded_size(4) == 3);
            CHECK(enc.decoded_size(8) == 6);
        }

        SECTION("over by 1")
        {
            CHECK(enc.decoded_size(5) == 3);
        }

        SECTION("over by 2")
        {
            CHECK(enc.decoded_size(6) == 3);
        }

        SECTION("over by 3")
        {
            CHECK(enc.decoded_size(7) == 3);
        }
    }

    SECTION("without padding")
    {
        encoding enc{net::encoding::base64::standard_alphabet(), 0};

        SECTION("under by 3")
        {
            CHECK(enc.decoded_size(1) == 0);
        }

        SECTION("under by 2")
        {
            CHECK(enc.decoded_size(2) == 1);
        }

        SECTION("under by 1")
        {
            CHECK(enc.decoded_size(3) == 2);
        }

        SECTION("multiple of 4")
        {
            CHECK(enc.decoded_size(4) == 3);
            CHECK(enc.decoded_size(8) == 6);
        }

        SECTION("over by 1")
        {
            CHECK(enc.decoded_size(5) == 3);
        }

        SECTION("over by 2")
        {
            CHECK(enc.decoded_size(6) == 4);
        }

        SECTION("over by 3")
        {
            CHECK(enc.decoded_size(7) == 5);
        }
    }
}

TEST_CASE("encode", "[encoding][base64]")
{
    SECTION("with padding")
    {
        encoding enc;

        SECTION("no padding needed")
        {
            auto input  = span_of("foo");
            auto actual = enc.encode(std::as_bytes(input));

            CHECK(actual == "Zm9v");
        }

        SECTION("two padding characters required")
        {
            auto input  = span_of("foo!");
            auto actual = enc.encode(std::as_bytes(input));

            CHECK(actual == "Zm9vIQ==");
        }

        SECTION("one padding character required")
        {
            auto input  = span_of("hello");
            auto actual = enc.encode(std::as_bytes(input));

            CHECK(actual == "aGVsbG8=");
        }
    }

    SECTION("without padding")
    {
        encoding enc{net::encoding::base64::standard_alphabet(), 0};

        SECTION("wouldn't need padding")
        {
            auto input  = span_of("foo");
            auto actual = enc.encode(std::as_bytes(input));

            CHECK(actual == "Zm9v");
        }

        SECTION("excludes two padding characters")
        {
            auto input  = span_of("foo!");
            auto actual = enc.encode(std::as_bytes(input));

            CHECK(actual == "Zm9vIQ");
        }

        SECTION("excludes one padding character")
        {
            auto input  = span_of("hello");
            auto actual = enc.encode(std::as_bytes(input));

            CHECK(actual == "aGVsbG8");
        }
    }
}

TEST_CASE("decode", "[encoding][base64]")
{
    SECTION("identifies invalid input")
    {
        encoding enc;

        auto actual = enc.decode("Zm=9");
        CHECK_FALSE(actual.has_value());
    }

    SECTION("with padding")
    {
        encoding enc;

        SECTION("no padding needed")
        {
            auto actual = enc.decode("Zm9v");

            CHECK(actual.has_value());
            CHECK(actual == "foo"_bytes);
        }

        SECTION("two padding characters")
        {
            auto actual = enc.decode("Zm9vIQ==");

            CHECK(actual.has_value());
            CHECK(actual == "foo!"_bytes);
        }

        SECTION("one padding character")
        {
            auto actual = enc.decode("aGVsbG8=");

            CHECK(actual.has_value());
            CHECK(actual == "hello"_bytes);
        }
    }

    SECTION("without padding")
    {
        encoding enc{net::encoding::base64::standard_alphabet(), 0};

        SECTION("wouldn't need padding")
        {
            auto actual = enc.decode("Zm9v");

            CHECK(actual.has_value());
            CHECK(actual == "foo"_bytes);
        }

        SECTION("excludes two padding characters")
        {
            auto actual = enc.decode("Zm9vIQ");

            CHECK(actual.has_value());
            CHECK(actual == "foo!"_bytes);
        }

        SECTION("excludes one padding character")
        {
            auto actual = enc.decode("aGVsbG8");

            CHECK(actual.has_value());
            CHECK(actual == "hello"_bytes);
        }
    }
}

TEST_CASE("decode rejects what encode doesn't make", "[encoding][base64][decode]")
{
    struct test_case
    {
        bool             padding;
        std::string_view input;
        const char*      why;
    };

    auto tc = GENERATE(test_case{.padding = true, .input = "Q", .why = "padded length not a multiple of 4"},
                       test_case{.padding = true, .input = "QQ", .why = "padding missing"},
                       test_case{.padding = true, .input = "QUI", .why = "padding missing"},
                       test_case{.padding = true, .input = "=", .why = "only padding"},
                       test_case{.padding = true, .input = "====", .why = "only padding"},
                       test_case{.padding = true, .input = "Q===", .why = "three padding characters"},
                       test_case{.padding = true, .input = "QQ=A", .why = "padding before the end"},
                       test_case{.padding = true, .input = "QQ==QUJD", .why = "padding before the end"},
                       test_case{.padding = true, .input = "QR==", .why = "unused bits set, after 1 byte"},
                       test_case{.padding = true, .input = "QUJ=", .why = "unused bits set, after 2 bytes"},
                       test_case{.padding = true, .input = "QU\xff=", .why = "not ASCII"},
                       test_case{.padding = true, .input = "QUJD\x80UJD", .why = "not ASCII"},
                       test_case{.padding = true, .input = "QU JD", .why = "whitespace"},
                       test_case{.padding = true, .input = "QUJD\nQUJD", .why = "a line break"},
                       test_case{.padding = true, .input = "QUJ-", .why = "another alphabet's character"},
                       test_case{.padding = false, .input = "Q", .why = "one more than a multiple of 4"},
                       test_case{.padding = false, .input = "QUJDR", .why = "one more than a multiple of 4"},
                       test_case{.padding = false, .input = "QQ==", .why = "padding"},
                       test_case{.padding = false, .input = "QR", .why = "unused bits set, after 1 byte"},
                       test_case{.padding = false, .input = "QUJ", .why = "unused bits set, after 2 bytes"});
    CAPTURE(tc.input, tc.why);

    const encoding enc{net::encoding::base64::standard_alphabet(), tc.padding ? '=' : '\0'};

    auto actual = enc.decode(tc.input);
    REQUIRE_FALSE(actual.has_value());
    CHECK(actual.error() == std::errc::invalid_argument);
}

TEST_CASE("decode accepts the shortest encodings", "[encoding][base64][decode]")
{
    CHECK(encoding{}.decode("") == ""_bytes);
    CHECK(encoding{}.decode("QQ==") == "A"_bytes);
    CHECK(encoding{}.decode("QUI=") == "AB"_bytes);
    CHECK(encoding{net::encoding::base64::standard_alphabet(), 0}.decode("QQ") == "A"_bytes);
    CHECK(encoding{net::encoding::base64::standard_alphabet(), 0}.decode("QUI") == "AB"_bytes);
}

TEST_CASE("decode_to writes into a buffer", "[encoding][base64][decode]")
{
    const encoding enc;

    std::array<std::byte, 5> out{};

    auto wrote = enc.decode_to("aGVsbG8=", out);
    REQUIRE(wrote.has_value());
    CHECK(*wrote == 5);
    CHECK(std::vector<std::byte>(out.begin(), out.end()) == "hello"_bytes);

    // decoded_size(8) is 6, but 5 is enough for these.
    std::array<std::byte, 4> small{};

    auto too_small = enc.decode_to("aGVsbG8=", small);
    REQUIRE_FALSE(too_small.has_value());
    CHECK(too_small.error() == std::errc::value_too_large);
}

TEST_CASE("decode undoes encode", "[encoding][base64][decode]")
{
    for (const char padchar : {'=', '\0'})
    {
        for (const auto& alphabet : {net::encoding::base64::standard_alphabet(), net::encoding::base64::url_alphabet()})
        {
            const encoding enc{alphabet, padchar};

            // every length up to 64, of every byte value.
            std::vector<std::byte> data;
            for (std::size_t length = 0; length <= 64; ++length)
            {
                CAPTURE(padchar, length);

                auto encoded = enc.encode(data);
                auto decoded = enc.decode(encoded);
                REQUIRE(decoded.has_value());
                CHECK(*decoded == data);

                data.push_back(static_cast<std::byte>((length * 131) + 7));
            }
        }
    }
}
