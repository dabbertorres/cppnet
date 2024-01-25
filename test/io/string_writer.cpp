#include "io/string_writer.hpp"

#include <exception> // IWYU pragma: keep
#include <string_view>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace std::string_view_literals;

TEST_CASE("build a string", "[io][string_writer]")
{
    net::io::string_writer<char> writer;

    auto res = writer.write("foo"sv);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);

    res = writer.write("bar"sv);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);

    res = writer.write("xyzzy"sv);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 5);

    auto out = writer.build();
    REQUIRE(out == "foobarxyzzy"sv);
}

TEST_CASE("build an empty string", "[io][string_writer]")
{
    net::io::string_writer<char> writer;

    auto out = writer.build();
    REQUIRE(out.empty());
}
