#include "encoding/hex.hpp"

#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <catch.hpp>

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

template<typename T, std::size_t N>
// NOLINTNEXTLINE(modernize-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
constexpr auto span_of(const T (&str)[N]) noexcept
{
    auto out = std::span<const T>{str};

    if constexpr (std::same_as<T, char>)
    {
        return out.subspan(0, out.size() - 1); // trim null terminator
    }

    return out;
}

}

using net::encoding::hex::decode;
using net::encoding::hex::encode;

TEST_CASE("encode", "[encoding][hex]")
{
    struct test_case
    {
        std::string            name;
        std::vector<std::byte> input;
        std::string            expect;
    };

    auto tc = GENERATE(
        test_case{
            .name   = "basic happy path",
            .input  = "foo"_bytes,
            .expect = "666F6F",
        },
        test_case{
            .name   = "empty",
            .input  = ""_bytes,
            .expect = "",
        });

    SECTION(tc.name)
    {
        CHECK(encode({tc.input}) == tc.expect);
    }
}

TEST_CASE("decode", "[encoding][hex]")
{
    SECTION("identifies invalid input")
    {
        SECTION("invalid characters")
        {
            auto result = decode(span_of("WXYZ"));
            CHECK_FALSE(result.has_value());
        }

        SECTION("missing characters")
        {
            auto result = decode(span_of("ABC"));
            CHECK_FALSE(result.has_value());
        }
    }

    struct test_case
    {
        std::string            name;
        std::string            input;
        std::vector<std::byte> expect;
    };

    auto tc = GENERATE(
        test_case{
            .name   = "uppercase",
            .input  = "666F6F",
            .expect = "foo"_bytes,
        },
        test_case{
            .name   = "lowercase",
            .input  = "666f6f",
            .expect = "foo"_bytes,
        },
        test_case{
            .name   = "empty",
            .input  = "",
            .expect = ""_bytes,
        });

    SECTION(tc.name)
    {
        auto result = decode({tc.input});
        CHECK(result.has_value());
        CHECK(result == tc.expect);
    }
}
