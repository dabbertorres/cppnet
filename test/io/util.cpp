#include "io/util.hpp"

#include <string_view>
#include <utility>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "io/buffered_reader.hpp"
#include "io/string_reader.hpp"

using namespace std::string_view_literals;

TEST_CASE("empty", "[io][readline]")
{
    net::io::string_reader   reader(""sv);
    net::io::buffered_reader buffer(&reader);

    auto task = net::io::readline(&buffer);
    auto res  = std::move(task).operator co_await().await_resume();
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line.empty());
}

TEST_CASE("no newline character", "[io][readline]")
{
    net::io::string_reader   reader("foobar"sv);
    net::io::buffered_reader buffer(&reader);

    auto task = net::io::readline(&buffer);
    auto res  = std::move(task).operator co_await().await_resume();
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line == "foobar"sv);

    auto task2 = net::io::readline(&buffer);
    auto res2  = std::move(task2).operator co_await().await_resume();
    REQUIRE(res2.has_value());

    auto line2 = res2.to_value();
    CHECK(line2.empty());
}

TEST_CASE("single line", "[io][readline]")
{
    net::io::string_reader   reader("foobar\r\n"sv);
    net::io::buffered_reader buffer(&reader);

    auto task = net::io::readline(&buffer);
    auto res  = std::move(task).operator co_await().await_resume();
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line == "foobar"sv);

    auto task2 = net::io::readline(&buffer);
    auto res2  = std::move(task2).operator co_await().await_resume();
    REQUIRE(res2.has_value());

    auto line2 = res2.to_value();
    CHECK(line2.empty());
}

TEST_CASE("multiple lines", "[io][readline]")
{
    net::io::string_reader   reader("foo\r\nbar\r\n"sv);
    net::io::buffered_reader buffer(&reader);

    auto task = net::io::readline(&buffer);
    auto res  = std::move(task).operator co_await().await_resume();
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line == "foo"sv);

    auto task2 = net::io::readline(&buffer);
    auto res2  = std::move(task2).operator co_await().await_resume();
    REQUIRE(res2.has_value());

    auto line2 = res2.to_value();
    CHECK(line2 == "bar"sv);

    auto task3 = net::io::readline(&buffer);
    auto res3  = std::move(task2).operator co_await().await_resume();
    REQUIRE(res3.has_value());

    auto line3 = res3.to_value();
    CHECK(line3.empty());
}
