#include "io/util.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/string_reader.hpp"

using namespace std::string_view_literals;

namespace
{

// timing_out_reader reads its content, and then times out (as a socket with a deadline would).
class timing_out_reader : public net::io::reader
{
public:
    explicit timing_out_reader(std::string_view content)
        : content{content}
    {}

    net::coro::task<net::io::result> read(std::span<std::byte> data) override
    {
        if (content.empty()) co_return {.count = 0, .err = std::make_error_code(std::errc::timed_out)};

        auto count = std::min(data.size(), content.size());
        std::ranges::copy(std::as_bytes(std::span{content.substr(0, count)}), data.begin());
        content.remove_prefix(count);
        co_return {.count = count};
    }

    using net::io::reader::read;

    [[nodiscard]] int native_handle() const noexcept override { return -1; }

private:
    std::string_view content;
};

}

TEST_CASE("empty", "[io][readline]")
{
    net::io::string_reader   reader(""sv);
    net::io::buffered_reader buffer(&reader);

    auto task = net::io::readline(&buffer);
    auto res  = test::sync_wait(std::move(task));
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line.empty());
}

TEST_CASE("no newline character", "[io][readline]")
{
    net::io::string_reader   reader("foobar"sv);
    net::io::buffered_reader buffer(&reader);

    auto task = net::io::readline(&buffer);
    auto res  = test::sync_wait(std::move(task));
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line == "foobar"sv);

    auto task2 = net::io::readline(&buffer);
    auto res2  = test::sync_wait(std::move(task2));
    REQUIRE(res2.has_value());

    auto line2 = res2.to_value();
    CHECK(line2.empty());
}

TEST_CASE("single line", "[io][readline]")
{
    net::io::string_reader   reader("foobar\r\n"sv);
    net::io::buffered_reader buffer(&reader);

    auto task = net::io::readline(&buffer);
    auto res  = test::sync_wait(std::move(task));
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line == "foobar"sv);

    auto task2 = net::io::readline(&buffer);
    auto res2  = test::sync_wait(std::move(task2));
    REQUIRE(res2.has_value());

    auto line2 = res2.to_value();
    CHECK(line2.empty());
}

TEST_CASE("multiple lines", "[io][readline]")
{
    net::io::string_reader   reader("foo\r\nbar\r\n"sv);
    net::io::buffered_reader buffer(&reader);

    auto task = net::io::readline(&buffer);
    auto res  = test::sync_wait(std::move(task));
    REQUIRE(res.has_value());

    auto line = res.to_value();
    CHECK(line == "foo"sv);

    auto task2 = net::io::readline(&buffer);
    auto res2  = test::sync_wait(std::move(task2));
    REQUIRE(res2.has_value());

    auto line2 = res2.to_value();
    CHECK(line2 == "bar"sv);

    auto task3 = net::io::readline(&buffer);
    auto res3  = test::sync_wait(std::move(task3));
    REQUIRE(res3.has_value());

    auto line3 = res3.to_value();
    CHECK(line3.empty());
}

TEST_CASE("errors other than the end of the stream are reported", "[io][readline]")
{
    // e.g. a slow client, which only sent part of a line before its deadline.
    timing_out_reader        reader("GET / HT"sv);
    net::io::buffered_reader buffer(&reader);

    auto res = test::sync_wait(net::io::readline(&buffer));
    REQUIRE(res.has_error());
    CHECK(res.to_error() == std::errc::timed_out);
}

TEST_CASE("terminated lines must end, and fit", "[io][read_terminated_line]")
{
    const auto read = [](std::string_view text, std::size_t max)
    {
        net::io::string_reader   reader(text);
        net::io::buffered_reader buffer(&reader);
        return test::sync_wait(net::io::read_terminated_line(&buffer, max));
    };

    CHECK(read("foo\r\nbar"sv, 5).to_value() == "foo");
    CHECK(read("\r\n"sv, 2).to_value().empty());

    CHECK(read("foo"sv, 100).to_error() == net::io::errc::end_of_stream);
    CHECK(read(""sv, 100).to_error() == net::io::errc::end_of_stream);
    CHECK(read("foo\r\n"sv, 4).to_error() == std::errc::value_too_large);

    // a bare LF doesn't end the line.
    CHECK(read("foo\nbar\r\n"sv, 100).to_value() == "foo\nbar");
}

TEST_CASE("long lines don't take a coroutine per byte", "[io][readline][read_terminated_line]")
{
    // reading a byte per co_await deepens the stack with each byte, where symmetric transfer isn't a tail call (e.g.
    // GCC's debug builds), until it overflows.
    const std::string long_line(1'000'000, 'x');
    const std::string text = long_line + "\r\nnext";

    {
        net::io::string_reader   reader{std::string_view{text}};
        net::io::buffered_reader buffer{&reader};

        auto res = test::sync_wait(net::io::readline(&buffer));
        REQUIRE(res.has_value());
        CHECK(res.to_value() == long_line);
    }

    {
        net::io::string_reader   reader{std::string_view{text}};
        net::io::buffered_reader buffer{&reader};

        auto res = test::sync_wait(net::io::read_terminated_line(&buffer, 2'000'000));
        REQUIRE(res.has_value());
        CHECK(res.to_value() == long_line);
    }
}
