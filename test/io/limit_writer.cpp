#include "io/limit_writer.hpp"

#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "io/string_writer.hpp"
#include "waiting_io.hpp"

using namespace std::string_view_literals;

TEST_CASE("writes up to the limit", "[io][limit_writer]")
{
    net::io::string_writer<char> out;
    net::io::limit_writer        limited{&out, 5};
    CHECK(limited.remaining() == 5);

    auto res = test::sync_wait(limited.write("abc"sv));
    CHECK_FALSE(res.err);
    CHECK(res.count == 3);
    CHECK(limited.remaining() == 2);

    res = test::sync_wait(limited.write("de"sv));
    CHECK_FALSE(res.err);
    CHECK(res.count == 2);
    CHECK(limited.remaining() == 0);

    CHECK(out.build() == "abcde");
}

TEST_CASE("writing past the limit writes what fits, and fails", "[io][limit_writer]")
{
    net::io::string_writer<char> out;
    net::io::limit_writer        limited{&out, 4};

    auto res = test::sync_wait(limited.write("abcdef"sv));
    CHECK(res.err == std::errc::value_too_large);
    CHECK(res.count == 4);
    CHECK(limited.remaining() == 0);

    res = test::sync_wait(limited.write("g"sv));
    CHECK(res.err == std::errc::value_too_large);
    CHECK(res.count == 0);

    // an empty write is fine, even at the limit.
    res = test::sync_wait(limited.write(""sv));
    CHECK_FALSE(res.err);

    CHECK(out.build() == "abcd");
}

TEST_CASE("a limit_writer's write that waits counts what it wrote once it's done", "[io][limit_writer][wait]")
{
    test::held_writer     held;
    net::io::limit_writer limited{&held, 4};

    SECTION("all of it")
    {
        auto writing = test::started(limited.write("abc"sv));
        REQUIRE_FALSE(writing.is_ready());

        // another while it waits is busy, writing nothing.
        auto busy = limited.write("d"sv);
        REQUIRE(busy.finished_without_coroutine());
        const auto busy_res = std::move(busy).result();
        CHECK(busy_res.err == std::errc::device_or_resource_busy);

        held.make_room(10);
        REQUIRE(writing.is_ready());
        const auto res = std::move(writing).result();
        CHECK_FALSE(res.err);
        CHECK(res.count == 3);
        CHECK(limited.remaining() == 1);
    }

    SECTION("past the limit: what fits, and a failure")
    {
        auto writing = test::started(limited.write("abcdef"sv));
        REQUIRE_FALSE(writing.is_ready());

        held.make_room(10);
        REQUIRE(writing.is_ready());
        const auto res = std::move(writing).result();
        CHECK(res.err == std::errc::value_too_large);
        CHECK(res.count == 4);
        CHECK(limited.remaining() == 0);
        CHECK(held.out == "abcd"sv);
    }

    SECTION("less than it asked for: no failure of the limit's")
    {
        auto writing = test::started(limited.write("abcdef"sv));
        REQUIRE_FALSE(writing.is_ready());

        held.make_room(2);
        REQUIRE(writing.is_ready());
        const auto res = std::move(writing).result();
        CHECK_FALSE(res.err);
        CHECK(res.count == 2);
        CHECK(limited.remaining() == 2);
    }
}
