#include "util/cache.hpp"

#include <exception> // IWYU pragma: keep

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

using net::util::cache;

TEST_CASE("set when empty", "[util][cache][set]")
{
    cache<int, int> c{4};

    c.set(1, 1);

    REQUIRE_FALSE(c.empty());
    REQUIRE(c.size() == 1);
    REQUIRE(c.get(1).value_or(0) == 1);
}

TEST_CASE("set until full", "[util][cache][set]")
{
    cache<int, int> c{4};

    c.set(1, 1);
    c.set(2, 2);
    c.set(3, 3);
    c.set(4, 4);

    REQUIRE_FALSE(c.empty());
    REQUIRE(c.size() == 4);
    REQUIRE(c.get(1).value_or(0) == 1);
    REQUIRE(c.get(2).value_or(0) == 2);
    REQUIRE(c.get(3).value_or(0) == 3);
    REQUIRE(c.get(4).value_or(0) == 4);
}

TEST_CASE("set when full", "[util][cache][set]")
{
    cache<int, int> c{4};

    c.set(1, 1);
    c.set(2, 2);
    c.set(3, 3);
    c.set(4, 4);
    c.set(5, 5);

    REQUIRE_FALSE(c.empty());
    REQUIRE(c.size() == 4);
    REQUIRE(c.get(1).value_or(0) == 0);
    REQUIRE(c.get(2).value_or(0) == 2);
    REQUIRE(c.get(3).value_or(0) == 3);
    REQUIRE(c.get(4).value_or(0) == 4);
    REQUIRE(c.get(5).value_or(0) == 5);
}

TEST_CASE("set when full after usage", "[util][cache][set]")
{
    cache<int, int> c{4};

    c.set(1, 1);
    c.set(2, 2);
    c.set(3, 3);
    c.set(4, 4);

    c.get(2);
    c.get(3);
    c.get(1);

    c.set(5, 5);

    REQUIRE_FALSE(c.empty());
    REQUIRE(c.size() == 4);
    REQUIRE(c.get(1).value_or(0) == 1);
    REQUIRE(c.get(2).value_or(0) == 2);
    REQUIRE(c.get(3).value_or(0) == 3);
    REQUIRE(c.get(4).value_or(0) == 0);
    REQUIRE(c.get(5).value_or(0) == 5);
}
