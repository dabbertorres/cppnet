#include "io/util.hpp"

#include <string_view>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "io/string_reader.hpp"

using namespace std::string_view_literals;

TEST_CASE("empty", "[io][readline]")
{
    net::io::string_reader   reader(""sv);
    net::io::buffered_reader buffer(&reader);

    auto res = net::io::readline(buffer);
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line.empty());
}

TEST_CASE("no newline character", "[io][readline]")
{
    net::io::string_reader   reader("foobar"sv);
    net::io::buffered_reader buffer(&reader);

    auto res = net::io::readline(buffer);
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line == "foobar"sv);

    auto res2 = net::io::readline(buffer);
    REQUIRE(res2.has_value());

    auto line2 = res2.to_value();
    CHECK(line2.empty());
}

TEST_CASE("single line", "[io][readline]")
{
    net::io::string_reader   reader("foobar\r\n"sv);
    net::io::buffered_reader buffer(&reader);

    auto res = net::io::readline(buffer);
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line == "foobar"sv);

    auto res2 = net::io::readline(buffer);
    REQUIRE(res2.has_value());

    auto line2 = res2.to_value();
    CHECK(line2.empty());
}

TEST_CASE("multiple lines", "[io][readline]")
{
    net::io::string_reader   reader("foo\r\nbar\r\n"sv);
    net::io::buffered_reader buffer(&reader);

    auto res = net::io::readline(buffer);
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line == "foo"sv);

    auto res2 = net::io::readline(buffer);
    REQUIRE(res2.has_value());

    auto line2 = res2.to_value();
    CHECK(line2 == "bar"sv);

    auto res3 = net::io::readline(buffer);
    REQUIRE(res3.has_value());

    auto line3 = res3.to_value();
    CHECK(line3.empty());
}
