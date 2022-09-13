#include "io/buffered_reader.hpp"

#include <string>
#include <string_view>

#include <catch.hpp>

#include "io/string_reader.hpp"

using namespace std::string_view_literals;

TEST_CASE("buffer reads to capacity as needed", "[io][buffered_reader]")
{
    net::io::string_reader   string("foobarbaz"sv);
    net::io::buffered_reader reader(string, 4);

    std::string buf(6, 0);

    auto res = reader.read(buf.data(), buf.size());
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 6);
    REQUIRE(buf == "foobar"sv);

    REQUIRE(reader.peek() == 'b');

    res = reader.read(buf.data(), buf.size());
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);
    REQUIRE(buf.substr(0, res.count) == "baz"sv);

    size_t just_expect_zero = 1;
    REQUIRE(reader.peek(just_expect_zero) == nullptr);
    REQUIRE(just_expect_zero == 0);
}

TEST_CASE("0-length read", "[io][buffered_reader]")
{
    net::io::string_reader   string("foobarbaz"sv);
    net::io::buffered_reader reader(string, 4);

    std::string buf(6, 0);

    auto res = reader.read(buf.data(), 0);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 0);
}
