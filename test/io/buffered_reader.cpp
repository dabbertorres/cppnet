#include "io/buffered_reader.hpp"

#include <string>
#include <string_view>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "io/string_reader.hpp"

using namespace std::string_view_literals;

TEST_CASE("buffer reads to capacity as needed", "[io][buffered_reader]")
{
    net::io::string_reader   string("foobarbaz"sv);
    net::io::buffered_reader reader(&string, 4);

    std::string buf(6, 0);

    auto res = reader.read(buf).operator co_await().await_resume();
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 6);
    REQUIRE(buf == "foobar"sv);

    auto [next, have_next] = reader.peek().operator co_await().await_resume();
    REQUIRE(have_next);
    REQUIRE(static_cast<char>(next) == 'b');

    res = reader.read(buf).operator co_await().await_resume();
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);
    REQUIRE(buf.substr(0, res.count) == "baz"sv);

    auto [next_again, have_next_again] = reader.peek().operator co_await().await_resume();
    REQUIRE_FALSE(have_next_again);
    REQUIRE(static_cast<char>(next_again) == 0);
}

TEST_CASE("0-length read", "[io][buffered_reader]")
{
    net::io::string_reader   string("foobarbaz"sv);
    net::io::buffered_reader reader(&string, 4);

    std::string buf;

    auto res = reader.read(buf).operator co_await().await_resume();
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 0);
}
