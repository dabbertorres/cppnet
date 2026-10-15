#include "io/buffer.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"

using namespace std::string_view_literals;

namespace
{

// clobber_stack overwrites the stack space a just-returned call was using.
[[gnu::noinline]] void clobber_stack()
{
    std::array<volatile char, 256> junk{};
    for (auto& c : junk) c = 'Z';
}

}

TEST_CASE("writes append", "[io][buffer]")
{
    net::io::buffer buf;

    REQUIRE(test::sync_wait(buf.write("abc"sv)).count == 3);
    REQUIRE(test::sync_wait(buf.write("de"sv)).count == 2);

    CHECK(buf.to_string() == "abcde"sv);
}

TEST_CASE("reads consume writes in order", "[io][buffer]")
{
    net::io::buffer buf;
    test::sync_wait(buf.write("abcde"sv));

    std::string out(3, 0);

    auto res = test::sync_wait(buf.read(out));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);
    CHECK(out == "abc"sv);

    res = test::sync_wait(buf.read(out));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 2);
    CHECK(out.substr(0, res.count) == "de"sv);

    res = test::sync_wait(buf.read(out));
    CHECK(res.err == net::io::status_condition::closed);
    CHECK(res.count == 0);
}

TEST_CASE("single character writes", "[io][buffer]")
{
    net::io::buffer  buf;
    net::io::writer& writer = buf;

    // the task must not refer to write()'s parameter, which is gone once write() returns.
    auto task = writer.write('x');
    clobber_stack();
    test::sync_wait(std::move(task));

    CHECK(buf.to_string() == "x"sv);
}
