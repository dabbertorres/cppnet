#include "util/string_util.hpp"

#include <sstream>
#include <string_view>

#include <catch.hpp>

using namespace std::literals::string_view_literals;

TEST_CASE("split on '/'", "[util][split_string]")
{
    auto gen = net::util::split_string("/foo/bar/baz"sv, '/');

    REQUIRE(gen);
    REQUIRE(gen() == ""sv);
    REQUIRE(gen);
    REQUIRE(gen() == "foo"sv);
    REQUIRE(gen);
    REQUIRE(gen() == "bar"sv);
    REQUIRE(gen);
    REQUIRE(gen() == "baz"sv);

    REQUIRE_FALSE(gen);
}

TEST_CASE("split on '/', non-zero start", "[util][split_string]")
{
    auto gen = net::util::split_string("/foo/bar/baz"sv, '/', 1);

    REQUIRE(gen);
    REQUIRE(gen() == "foo"sv);
    REQUIRE(gen);
    REQUIRE(gen() == "bar"sv);
    REQUIRE(gen);
    REQUIRE(gen() == "baz"sv);

    REQUIRE_FALSE(gen);
}

TEST_CASE("split on '/', trailing separator", "[util][split_string]")
{
    auto gen = net::util::split_string("/foo/bar/baz/"sv, '/');

    REQUIRE(gen);
    REQUIRE(gen() == ""sv);
    REQUIRE(gen);
    REQUIRE(gen() == "foo"sv);
    REQUIRE(gen);
    REQUIRE(gen() == "bar"sv);
    REQUIRE(gen);
    REQUIRE(gen() == "baz"sv);
    REQUIRE(gen);
    REQUIRE(gen() == ""sv);

    REQUIRE_FALSE(gen);
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
