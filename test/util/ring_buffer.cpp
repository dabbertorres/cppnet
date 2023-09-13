#include "util/ring_buffer.hpp"

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

using net::util::ring_buffer;

TEST_CASE("push to empty", "[util][ring_buffer][push]")
{
    ring_buffer<std::size_t, 4> buf;

    buf.push(5);

    REQUIRE_FALSE(buf.empty());
    REQUIRE(buf.size() == 1);
    REQUIRE(buf[0] == 5);
}

TEST_CASE("push until full", "[util][ring_buffer][push]")
{
    ring_buffer<std::size_t, 4> buf;

    for (auto i = 0u; i < buf.capacity(); ++i)
    {
        buf.push(i + 1);
    }

    REQUIRE_FALSE(buf.empty());
    REQUIRE(buf.size() == 4);

    REQUIRE(buf[0] == 1);
    REQUIRE(buf[1] == 2);
    REQUIRE(buf[2] == 3);
    REQUIRE(buf[3] == 4);
}

TEST_CASE("wrap around", "[util][ring_buffer][push]")
{
    ring_buffer<std::size_t, 4> buf;

    for (auto i = 0u; i < buf.capacity() + 2; ++i)
    {
        buf.push(i + 1);
    }

    REQUIRE_FALSE(buf.empty());
    REQUIRE(buf.size() == 4);

    REQUIRE(buf[0] == 3);
    REQUIRE(buf[1] == 4);
    REQUIRE(buf[2] == 5);
    REQUIRE(buf[3] == 6);
}

TEST_CASE("pop empty", "[util][ring_buffer][pop]")
{
    ring_buffer<int, 4> buf;

    buf.pop();

    REQUIRE(buf.empty());
    REQUIRE(buf.size() == 0);
}

TEST_CASE("pop full", "[util][ring_buffer][pop]")
{
    ring_buffer<std::size_t, 4> buf;

    for (auto i = 0u; i < buf.capacity(); ++i)
    {
        buf.push(i + 1);
    }

    REQUIRE_FALSE(buf.empty());
    REQUIRE(buf.size() == 4);

    REQUIRE(buf[0] == 1);
    REQUIRE(buf[1] == 2);
    REQUIRE(buf[2] == 3);
    REQUIRE(buf[3] == 4);

    buf.pop();

    REQUIRE_FALSE(buf.empty());
    REQUIRE(buf.size() == 3);

    REQUIRE(buf[0] == 2);
    REQUIRE(buf[1] == 3);
    REQUIRE(buf[2] == 4);
}

TEST_CASE("pop until empty", "[util][ring_buffer][pop]")
{
    ring_buffer<std::size_t, 4> buf;

    for (auto i = 0u; i < buf.capacity(); ++i)
    {
        buf.push(i + 1);
    }

    REQUIRE_FALSE(buf.empty());
    REQUIRE(buf.size() == 4);

    REQUIRE(buf[0] == 1);
    REQUIRE(buf[1] == 2);
    REQUIRE(buf[2] == 3);
    REQUIRE(buf[3] == 4);

    buf.pop();
    REQUIRE(buf[0] == 2);
    REQUIRE(buf.size() == 3);

    buf.pop();
    REQUIRE(buf[0] == 3);
    REQUIRE(buf.size() == 2);

    buf.pop();
    REQUIRE(buf[0] == 4);
    REQUIRE(buf.size() == 1);

    buf.pop();
    REQUIRE(buf.empty());
    REQUIRE(buf.size() == 0);
}
