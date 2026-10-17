#include "io/buffered_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/string_writer.hpp"
#include "io/writer.hpp"

namespace
{

// short_writer accepts at most 2 bytes per write, like a socket with a nearly full send buffer. Its writes finish
// without a coroutine (as a socket's that don't wait do), or with one (as one's that do).
class short_writer : public net::io::writer
{
public:
    explicit short_writer(bool without_coroutine) noexcept
        : without_coroutine{without_coroutine}
    {}

    net::coro::task<net::io::result> write(std::span<const std::byte> data) override
    {
        auto amount = std::min<std::size_t>(2, data.size());
        out.append(reinterpret_cast<const char*>(data.data()), amount);

        if (without_coroutine) return net::coro::ready_task<net::io::result>(net::io::result{.count = amount});
        return wrote(amount);
    }

    using net::io::writer::write;

    [[nodiscard]] int native_handle() const noexcept override { return -1; }

    std::string out;

private:
    static net::coro::task<net::io::result> wrote(std::size_t amount) { co_return {.count = amount}; }

    bool without_coroutine;
};

// stuck_writer breaks the writer contract by writing nothing, without reporting an error, with or without a coroutine.
class stuck_writer : public net::io::writer
{
public:
    explicit stuck_writer(bool without_coroutine) noexcept
        : without_coroutine{without_coroutine}
    {}

    net::coro::task<net::io::result> write(std::span<const std::byte> /*data*/) override
    {
        if (without_coroutine) return net::coro::ready_task<net::io::result>(net::io::result{.count = 0});
        return wrote_nothing();
    }

    using net::io::writer::write;

    [[nodiscard]] int native_handle() const noexcept override { return -1; }

private:
    static net::coro::task<net::io::result> wrote_nothing() { co_return {.count = 0}; }

    bool without_coroutine;
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

    const bool without_coroutine = GENERATE(true, false);
    CAPTURE(without_coroutine);

    short_writer             underlying{without_coroutine};
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

    const bool without_coroutine = GENERATE(true, false);
    CAPTURE(without_coroutine);

    stuck_writer             underlying{without_coroutine};
    net::io::buffered_writer writer{&underlying, 4};

    auto res = test::sync_wait(writer.write("ABCDEFGHIJ"sv));
    CHECK(res.err == net::io::errc::no_progress);

    // the unwritten data is kept, rather than dropped.
    CHECK(writer.size() == 4);
}
