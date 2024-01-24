#include "encoding/base64.hpp"

#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <span>
#include <vector>

#include <catch.hpp>

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

using encoding = net::encoding::base64::encoding;

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

        auto input  = span_of("Zm=9");
        auto actual = enc.decode(input);
        CHECK_FALSE(actual.has_value());
    }

    SECTION("with padding")
    {
        encoding enc;

        SECTION("no padding needed")
        {
            auto input  = span_of("Zm9v");
            auto actual = enc.decode(input);

            CHECK(actual.has_value());
            CHECK(actual == "foo"_bytes);
        }

        SECTION("two padding characters")
        {
            auto input  = span_of("Zm9vIQ==");
            auto actual = enc.decode(input);

            CHECK(actual.has_value());
            CHECK(actual == "foo!"_bytes);
        }

        SECTION("one padding character")
        {
            auto input  = span_of("aGVsbG8=");
            auto actual = enc.decode(input);

            CHECK(actual.has_value());
            CHECK(actual == "hello"_bytes);
        }
    }

    SECTION("without padding")
    {
        encoding enc{net::encoding::base64::standard_alphabet(), 0};

        SECTION("wouldn't need padding")
        {
            auto input  = span_of("Zm9v");
            auto actual = enc.decode(input);

            CHECK(actual.has_value());
            CHECK(actual == "foo"_bytes);
        }

        SECTION("excludes two padding characters")
        {
            auto input  = span_of("Zm9vIQ");
            auto actual = enc.decode(input);

            CHECK(actual.has_value());
            CHECK(actual == "foo!"_bytes);
        }

        SECTION("excludes one padding character")
        {
            auto input  = span_of("aGVsbG8");
            auto actual = enc.decode(input);

            CHECK(actual.has_value());
            CHECK(actual == "hello"_bytes);
        }
    }
}
