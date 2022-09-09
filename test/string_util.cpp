#include "string_util.hpp"

#include <sstream>
#include <string_view>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

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
