#include "io/string_writer.hpp"

#include <exception> // IWYU pragma: keep
#include <string_view>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"

using namespace std::string_view_literals;

TEST_CASE("build a string", "[io][string_writer]")
{
    net::io::string_writer<char> writer;

    auto res = test::sync_wait(writer.write("foo"sv));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);

    res = test::sync_wait(writer.write("bar"sv));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);

    res = test::sync_wait(writer.write("xyzzy"sv));
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

TEST_CASE("write single characters", "[io][string_writer]")
{
    net::io::string_writer<char> writer;

    writer.write('a');
    REQUIRE_FALSE(test::sync_wait(writer.write("bc"sv)).err);
    writer.write('d');

    CHECK(writer.build() == "abcd");
    CHECK(writer.length() == 4);
}
