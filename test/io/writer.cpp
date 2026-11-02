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
