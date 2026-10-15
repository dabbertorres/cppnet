#include "util/string_util.hpp"

#include <exception> // IWYU pragma: keep
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::literals::string_view_literals;

namespace
{

template<typename Parts>
std::vector<std::string_view> collect(const Parts& parts)
{
    std::vector<std::string_view> out;
    for (auto part : parts) out.push_back(part);
    return out;
}

}

static_assert(std::ranges::input_range<decltype(net::util::split_string("a/b"sv, '/'))>);

TEST_CASE("split on '/'", "[util][split_string]")
{
    CHECK(collect(net::util::split_string("/foo/bar/baz"sv, '/')) == std::vector{""sv, "foo"sv, "bar"sv, "baz"sv});
}

TEST_CASE("split on '/', non-zero start", "[util][split_string]")
{
    CHECK(collect(net::util::split_string("/foo/bar/baz"sv, '/', 1)) == std::vector{"foo"sv, "bar"sv, "baz"sv});

    // a start at the end is an empty part, and one past it none.
    CHECK(collect(net::util::split_string("a/"sv, '/', 2)) == std::vector{""sv});
    CHECK(collect(net::util::split_string("a/"sv, '/', 3)).empty());
}

TEST_CASE("split on '/', trailing separator", "[util][split_string]")
{
    CHECK(collect(net::util::split_string("/foo/bar/baz/"sv, '/'))
          == std::vector{""sv, "foo"sv, "bar"sv, "baz"sv, ""sv});
}

TEST_CASE("split with empty parts", "[util][split_string]")
{
    CHECK(collect(net::util::split_string("a,,b"sv, ',')) == std::vector{"a"sv, ""sv, "b"sv});
    CHECK(collect(net::util::split_string(","sv, ',')) == std::vector{""sv, ""sv});

    // unlike std::views::split, an empty string is one empty part.
    CHECK(collect(net::util::split_string(""sv, ',')) == std::vector{""sv});
}

TEST_CASE("split without a separator", "[util][split_string]")
{
    CHECK(collect(net::util::split_string("foo"sv, '/')) == std::vector{"foo"sv});
}

TEST_CASE("split a temporary string", "[util][split_string]")
{
    // the parts keep their own copy of a string (rather than a view of a temporary).
    std::vector<std::string> parts;
    for (auto part : net::util::split_string(std::string{"foo/bar"}, '/')) parts.push_back(part);

    CHECK(parts == std::vector<std::string>{"foo", "bar"});
}

TEST_CASE("range-based for loop", "[util][split_string]")
{
    int i = 0;
    for (auto part : net::util::split_string("/foo/bar/baz"sv, '/'))
    {
        switch (i)
        {
        case 0: REQUIRE(part == ""sv); break;
        case 1: REQUIRE(part == "foo"sv); break;
        case 2: REQUIRE(part == "bar"sv); break;
        case 3: REQUIRE(part == "baz"sv); break;
        default: REQUIRE(false);
        }

        ++i;
    }

    REQUIRE(i == 4);
}

TEST_CASE("both lowercase", "[util][equal_ignore_case]")
{
    REQUIRE(net::util::equal_ignore_case("foobarbaz"sv, "foobarbaz"sv));
}

TEST_CASE("both uppercase", "[util][equal_ignore_case]")
{
    REQUIRE(net::util::equal_ignore_case("FOOBARBAZ"sv, "FOOBARBAZ"sv));
}

TEST_CASE("opposite case", "[util][equal_ignore_case]")
{
    REQUIRE(net::util::equal_ignore_case("foobarbaz"sv, "FOOBARBAZ"sv));
}

TEST_CASE("mixed case", "[util][equal_ignore_case]")
{
    REQUIRE(net::util::equal_ignore_case("FoObArBaZ"sv, "fOoBaRbAz"sv));
}

TEST_CASE("different", "[util][equal_ignore_case]")
{
    REQUIRE_FALSE(net::util::equal_ignore_case("xyzzy"sv, "foobarbaz"sv));
}

TEST_CASE("empty", "[util][trim_string]")
{
    auto actual = net::util::trim_string(""sv);
    REQUIRE(actual == ""sv);
}

TEST_CASE("already trimmed", "[util][trim_string]")
{
    auto actual = net::util::trim_string("foo"sv);
    REQUIRE(actual == "foo"sv);
}

TEST_CASE("single prefix", "[util][trim_string]")
{
    auto actual = net::util::trim_string(" foo"sv);
    REQUIRE(actual == "foo"sv);
}

TEST_CASE("single suffix", "[util][trim_string]")
{
    auto actual = net::util::trim_string("foo "sv);
    REQUIRE(actual == "foo"sv);
}

TEST_CASE("single prefix and suffix", "[util][trim_string]")
{
    auto actual = net::util::trim_string(" foo "sv);
    REQUIRE(actual == "foo"sv);
}

TEST_CASE("multiple prefix", "[util][trim_string]")
{
    auto actual = net::util::trim_string("  \tfoo"sv);
    REQUIRE(actual == "foo"sv);
}

TEST_CASE("multiple suffix", "[util][trim_string]")
{
    auto actual = net::util::trim_string("foo\t   "sv);
    REQUIRE(actual == "foo"sv);
}

TEST_CASE("multiple prefix and suffix", "[util][trim_string]")
{
    auto actual = net::util::trim_string("\t   foo\t   "sv);
    REQUIRE(actual == "foo"sv);
}

TEST_CASE("only whitespace", "[util][trim_string]")
{
    CHECK(net::util::trim_string(" \t\r\n "sv).empty());
    CHECK(net::util::trim_string(std::string{"  "}).empty());
}

TEST_CASE("multi-byte characters aren't trimmed", "[util][trim_string]")
{
    // U+0100 and U+2000 in UTF-8: their bytes used to be taken for (part of) the Unicode whitespace.
    CHECK(net::util::trim_string(" \xC4\x80 "sv) == "\xC4\x80"sv);
    CHECK(net::util::trim_string("\xE2\x80\x80"sv) == "\xE2\x80\x80"sv);
}

TEST_CASE("less_ignore_case orders ignoring case", "[util][less_ignore_case]")
{
    CHECK(net::util::less_ignore_case("abc"sv, "ABD"sv));
    CHECK_FALSE(net::util::less_ignore_case("ABD"sv, "abc"sv));
    CHECK_FALSE(net::util::less_ignore_case("abc"sv, "ABC"sv));
    CHECK(net::util::less_ignore_case("ab"sv, "ABC"sv));
}

TEST_CASE("multiple strings", "[util][join]")
{
    constexpr auto expect = "foo,bar,baz"sv;
    auto           actual = net::util::join(","sv, {"foo"sv, "bar"sv, "baz"sv});

    REQUIRE(expect == actual);
}

TEST_CASE("occurs once at front", "[util][replace]")
{
    auto actual = net::util::replace("barfoo"sv, "bar"sv, "foo"sv);

    REQUIRE("foofoo"sv == actual);
}

TEST_CASE("occurs once at end", "[util][replace]")
{
    auto actual = net::util::replace("foobar"sv, "bar"sv, "foo"sv);

    REQUIRE("foofoo"sv == actual);
}

TEST_CASE("whole string", "[util][replace]")
{
    auto actual = net::util::replace("bar"sv, "bar"sv, "foo"sv);

    REQUIRE("foo"sv == actual);
}

TEST_CASE("more than once", "[util][replace]")
{
    auto actual = net::util::replace("barfoobar"sv, "bar"sv, "foo"sv);

    REQUIRE("foofoofoo"sv == actual);
}

TEST_CASE("sequential", "[util][replace]")
{
    auto actual = net::util::replace("barbar"sv, "bar"sv, "foo"sv);

    REQUIRE("foofoo"sv == actual);
}
