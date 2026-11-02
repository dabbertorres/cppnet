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

TEST_CASE("a string_reader's reads finish without a coroutine", "[io][string_reader]")
{
    net::io::string_reader reader("foobar"sv);

    std::string buf(4, 0);

    // through the reader interface, as a byte span, and as a span of its characters.
    auto bytes = static_cast<net::io::reader&>(reader).read(std::as_writable_bytes(std::span{buf}));
    REQUIRE(bytes.finished_without_coroutine());
    const auto first = std::move(bytes).result();
    CHECK_FALSE(first.err);
    CHECK(first.count == 4);
    CHECK(buf == "foob"sv);

    auto chars = reader.read(std::span{buf});
    REQUIRE(chars.finished_without_coroutine());
    const auto second = std::move(chars).result();
    CHECK_FALSE(second.err);
    CHECK(second.count == 2);
    CHECK(std::string_view{buf}.substr(0, 2) == "ar"sv);

    auto end = reader.read(std::span{buf});
    REQUIRE(end.finished_without_coroutine());
    const auto third = std::move(end).result();
    CHECK(third.err == net::io::errc::end_of_stream);
}
