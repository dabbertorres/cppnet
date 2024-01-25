#include "io/string_reader.hpp"

#include <string>
#include <string_view>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace std::string_view_literals;

TEST_CASE("partial reading", "[io][string_reader]")
{
    net::io::string_reader reader("foobar"sv);

    std::string buf(3, 0);

    auto res = reader.read(buf);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);
    REQUIRE(buf == "foo"sv);

    res = reader.read(buf);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);
    REQUIRE(buf == "bar"sv);

    res = reader.read(buf);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 0);
    REQUIRE(buf == "bar"sv); // ensure it doesn't write anything at all
}

TEST_CASE("0-length read", "[io][string_reader]")
{
    net::io::string_reader reader("foobar"sv);

    std::string buf;

    auto res = reader.read(buf);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 0);
    REQUIRE(buf == ""sv);
}

TEST_CASE("full-length read", "[io][string_reader]")
{
    net::io::string_reader reader("foobar"sv);

    std::string buf(6, 0);

    auto res = reader.read(buf);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 6);
    REQUIRE(buf == "foobar"sv);

    res = reader.read(buf);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 0);
    REQUIRE(buf == "foobar"sv); // ensure it doesn't write anything at all
}
