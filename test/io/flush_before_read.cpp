#include "io/flush_before_read.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "waiting_io.hpp"

using namespace std::string_view_literals;

TEST_CASE("a read sends what's been written first", "[io][flush_before_read]")
{
    test::fed_reader           fed;
    test::held_writer          held;
    net::io::buffered_writer   writer{&held, 16};
    net::io::flush_before_read input{&fed, &writer};

    held.make_room(100);
    fed.feed("next request"sv);

    auto wrote = test::sync_wait(writer.write("response"sv));
    REQUIRE(wrote.count == 8);

    std::string buf(4, '\0');
    const auto  res = test::sync_wait(input.read(buf));
    CHECK_FALSE(res.err);
    CHECK(res.count == 4);
    CHECK(buf == "next"sv);
    CHECK(held.out == "response"sv);
}

TEST_CASE("a read whose flush waits reads once the flush is done", "[io][flush_before_read][wait]")
{
    test::fed_reader           fed;
    test::held_writer          held;
    net::io::buffered_writer   writer{&held, 16};
    net::io::flush_before_read input{&fed, &writer};

    auto wrote = test::sync_wait(writer.write("response"sv));
    REQUIRE(wrote.count == 8);

    std::string buf(4, '\0');

    SECTION("then the read waits too")
    {
        auto reading = test::started(input.read(buf));
        REQUIRE_FALSE(reading.is_ready());

        held.make_room(100);
        REQUIRE_FALSE(reading.is_ready());
        CHECK(held.out == "response"sv);
        CHECK(fed.waiting());

        fed.feed("next request"sv);
        REQUIRE(reading.is_ready());
        const auto res = std::move(reading).result();
        CHECK_FALSE(res.err);
        CHECK(res.count == 4);
        CHECK(buf == "next"sv);
    }

    SECTION("the read doesn't")
    {
        auto reading = test::started(input.read(buf));
        REQUIRE_FALSE(reading.is_ready());

        fed.feed("next request"sv);
        held.make_room(100);
        REQUIRE(reading.is_ready());
        const auto res = std::move(reading).result();
        CHECK(res.count == 4);
        CHECK(buf == "next"sv);
    }

    SECTION("a flush that fails ends the stream, without reading")
    {
        auto reading = test::started(input.read(buf));
        REQUIRE_FALSE(reading.is_ready());

        held.fail(std::make_error_code(std::errc::broken_pipe));
        REQUIRE(reading.is_ready());
        const auto res = std::move(reading).result();
        CHECK(res.err == net::io::errc::end_of_stream);
        CHECK(res.count == 0);
        CHECK(fed.asked.empty());
    }
}
