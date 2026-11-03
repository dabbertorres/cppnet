#include "io/writer.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "io/io.hpp"
#include "io/string_writer.hpp"
#include "waiting_io.hpp"

using namespace std::string_view_literals;

TEST_CASE("write_all of one piece is the writer's write, whether or not it waits", "[io][writer][write_all]")
{
    test::held_writer out;

    SECTION("a write the writer takes at once is a ready task")
    {
        out.make_room(5);

        auto written = net::io::write_all(out, "hello"sv);
        REQUIRE(written.finished_without_coroutine());
        const auto res = std::move(written).result();
        CHECK_FALSE(res.err);
        CHECK(res.count == 5);
        CHECK(out.out == "hello");
    }

    SECTION("a write that waits is the writer's operation, not a coroutine")
    {
        auto written = net::io::write_all(out, "hello"sv);
        REQUIRE_FALSE(written.finished_without_coroutine());
        CHECK_FALSE(written.get_handle());

        auto writing = test::started(std::move(written));
        REQUIRE_FALSE(writing.is_ready());
        REQUIRE(out.waiting());

        out.make_room(5);
        REQUIRE(writing.is_ready());
        const auto res = std::move(writing).result();
        CHECK_FALSE(res.err);
        CHECK(res.count == 5);
        CHECK(out.out == "hello");
    }
}

TEST_CASE("write_all writes its pieces in turn, until one fails", "[io][writer][write_all]")
{
    SECTION("all of them")
    {
        net::io::string_writer<char> out;

        const std::array<std::byte, 2> bytes{std::byte{'d'}, std::byte{'e'}};
        const auto res = test::sync_wait(net::io::write_all(out, "ab"sv, 'c', std::span{bytes}, std::byte{'f'}));
        CHECK_FALSE(res.err);
        CHECK(res.count == 6);
        CHECK(out.build() == "abcdef");
    }

    SECTION("up to one that fails")
    {
        test::held_writer out;
        out.make_room(2);

        auto writing = test::started(net::io::write_all(out, "ab"sv, "cd"sv, "ef"sv));
        REQUIRE_FALSE(writing.is_ready());

        out.fail(std::make_error_code(std::errc::broken_pipe));
        REQUIRE(writing.is_ready());
        const auto res = std::move(writing).result();
        CHECK(res.err == std::errc::broken_pipe);
        CHECK(res.count == 2);
        CHECK(out.out == "ab");
        CHECK(out.asked == std::vector<std::size_t>{2, 2});
    }
}
