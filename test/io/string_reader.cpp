#include "io/string_reader.hpp"

#include <string>
#include <string_view>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "io/io.hpp"

using namespace std::string_view_literals;

TEST_CASE("partial reading", "[io][string_reader]")
{
    net::io::string_reader reader("foobar"sv);

    std::string buf(3, 0);

    auto res = test::sync_wait(reader.read(buf));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);
    REQUIRE(buf == "foo"sv);

    res = test::sync_wait(reader.read(buf));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);
    REQUIRE(buf == "bar"sv);

    res = test::sync_wait(reader.read(buf));
    REQUIRE(res.err == net::io::errc::end_of_stream);
    REQUIRE(res.count == 0);
    REQUIRE(buf == "bar"sv); // ensure it doesn't write anything at all
}

TEST_CASE("0-length read", "[io][string_reader]")
{
    net::io::string_reader reader("foobar"sv);

    std::string buf;

    auto res = test::sync_wait(reader.read(buf));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 0);
    REQUIRE(buf.empty());
}

TEST_CASE("full-length read", "[io][string_reader]")
{
    net::io::string_reader reader("foobar"sv);

    std::string buf(6, 0);

    auto res = test::sync_wait(reader.read(buf));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 6);
    REQUIRE(buf == "foobar"sv);

    res = test::sync_wait(reader.read(buf));
    REQUIRE(res.err == net::io::errc::end_of_stream);
    REQUIRE(res.count == 0);
    REQUIRE(buf == "foobar"sv); // ensure it doesn't write anything at all
}
