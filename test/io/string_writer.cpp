#include "io/string_writer.hpp"

#include <exception> // IWYU pragma: keep

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("build a string", "[io][string_writer]")
{
    net::io::string_writer<char> writer;

    auto res = writer.write("foo");
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);

    res = writer.write("bar");
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);

    res = writer.write("xyzzy");
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 5);

    auto out = writer.build();
    REQUIRE(out == "foobarxyzzy");
}

TEST_CASE("build an empty string", "[io][string_writer]")
{
    net::io::string_writer<char> writer;

    auto out = writer.build();
    REQUIRE(out.empty());
}
