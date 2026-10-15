#include "io/buffered_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/string_writer.hpp"
#include "io/writer.hpp"

namespace
{

// short_writer accepts at most 2 bytes per write, like a socket with a nearly full send buffer.
class short_writer : public net::io::writer
{
public:
    net::coro::task<net::io::result> write(std::span<const std::byte> data) override
    {
        auto amount = std::min<std::size_t>(2, data.size());
        out.append(reinterpret_cast<const char*>(data.data()), amount);
        co_return {.count = amount};
    }

    using net::io::writer::write;

    [[nodiscard]] int native_handle() const noexcept override { return -1; }

    std::string out;
};

// stuck_writer breaks the writer contract by writing nothing, without reporting an error.
class stuck_writer : public net::io::writer
{
public:
    net::coro::task<net::io::result> write(std::span<const std::byte> /*data*/) override { co_return {.count = 0}; }

    using net::io::writer::write;

    [[nodiscard]] int native_handle() const noexcept override { return -1; }
};

}

TEST_CASE("buffer fills up and flushes", "[io][buffered_writer]")
{
    using namespace std::string_view_literals;

    net::io::string_writer<char> builder;
    net::io::buffered_writer     writer{&builder, 4};

    auto res = test::sync_wait(writer.write("foo"sv));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);

    // buffer hasn't flushed yet
    auto out = builder.build();
    REQUIRE(out.empty());

    res = test::sync_wait(writer.write("bar"sv));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 3);

    // should've flushed 4 characters
    out = builder.build();
    REQUIRE(out == "foob");

    // flush it all
    test::sync_wait(writer.flush());
    out = builder.build();
    REQUIRE(out == "foobar");
}

TEST_CASE("0-length write", "[io][buffered_writer]")
{
    using namespace std::string_view_literals;

    net::io::string_writer<char> builder;
    net::io::buffered_writer     writer{&builder, 4};

    auto res = test::sync_wait(writer.write(""sv));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 0);

    test::sync_wait(writer.flush());
    auto out = builder.build();
    REQUIRE(out.empty());
}

TEST_CASE("partial writes keep data in order", "[io][buffered_writer]")
{
    using namespace std::string_view_literals;

    short_writer             underlying;
    net::io::buffered_writer writer{&underlying, 4};

    auto res = test::sync_wait(writer.write("ABCDEFGHIJ"sv));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 10);

    res = test::sync_wait(writer.flush());
    REQUIRE_FALSE(res.err);
    CHECK(underlying.out == "ABCDEFGHIJ"sv);
    CHECK(writer.size() == 0);
}

TEST_CASE("writer that makes no progress is an error", "[io][buffered_writer]")
{
    using namespace std::string_view_literals;

    stuck_writer             underlying;
    net::io::buffered_writer writer{&underlying, 4};

    auto res = test::sync_wait(writer.write("ABCDEFGHIJ"sv));
    CHECK(res.err == net::io::status_condition::error);

    // the unwritten data is kept, rather than dropped.
    CHECK(writer.size() == 4);
}
