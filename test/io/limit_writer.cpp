#include "io/limit_writer.hpp"

#include <exception> // IWYU pragma: keep
#include <string_view>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "io/string_writer.hpp"

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
