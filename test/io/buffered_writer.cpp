#include "io/buffered_writer.hpp"

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "io/string_writer.hpp"

TEST_CASE("buffer fills up and flushes", "[io][buffered_writer]")
{
    net::io::string_writer<char> builder;
    net::io::buffered_writer     writer{builder, 4};

    auto res = writer.write("foo", 3);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);

    // buffer hasn't flushed yet
    auto out = builder.build();
    REQUIRE(out.empty());

    res = writer.write("bar", 3);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);

    // should've flushed 4 characters
    out = builder.build();
    REQUIRE(out == "foob");

    // flush it all
    writer.flush();
    out = builder.build();
    REQUIRE(out == "foobar");
}

TEST_CASE("0-length write", "[io][buffered_writer]")
{
    net::io::string_writer<char> builder;
    net::io::buffered_writer     writer{builder, 4};

    auto res = writer.write("foo", 0);
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 0);

    writer.flush();
    auto out = builder.build();
    REQUIRE(out.empty());
}
