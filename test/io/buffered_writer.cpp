#include "io/buffered_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/string_writer.hpp"
#include "io/writer.hpp"
#include "waiting_io.hpp"

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

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

    std::string out;

private:
    static net::coro::task<net::io::result> wrote(std::size_t amount) { co_return {.count = amount}; }

    bool without_coroutine;
};

// recording_writer writes all it's given, and records how much each write was.
class recording_writer : public net::io::writer
{
public:
    net::coro::task<net::io::result> write(std::span<const std::byte> data) override
    {
        sizes.push_back(data.size());
        out.append(reinterpret_cast<const char*>(data.data()), data.size());
        return net::coro::ready_task<net::io::result>(net::io::result{.count = data.size()});
    }

    using net::io::writer::write;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

    std::string              out;
    std::vector<std::size_t> sizes;
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

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

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

TEST_CASE("large write goes past the buffer at once", "[io][buffered_writer]")
{
    using namespace std::string_view_literals;

    recording_writer         underlying;
    net::io::buffered_writer writer{&underlying, 4};

    auto res = test::sync_wait(writer.write("AB"sv));
    REQUIRE_FALSE(res.err);

    // larger than the buffer: the buffer filled and flushed, then the rest in one write of the underlying writer.
    res = test::sync_wait(writer.write("CDEFGHIJKL"sv));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 10);
    CHECK(underlying.out == "ABCDEFGHIJKL"sv);
    CHECK(underlying.sizes == std::vector<std::size_t>{4, 8});
    CHECK(writer.size() == 0);
}

TEST_CASE("a write of more than the buffer holds, whose writes wait, flushes the buffer, then writes straight from it",
          "[io][buffered_writer][through]")
{
    using namespace std::string_view_literals;

    test::held_writer        held;
    net::io::buffered_writer writer{&held, 8};

    auto res = test::sync_wait(writer.write("AB"sv));
    REQUIRE(res.count == 2);

    // the buffer filled, and its flush waits.
    auto writing = test::started(writer.write("CDEFGHIJKLMNOPQRSTUV"sv));
    REQUIRE_FALSE(writing.is_ready());
    CHECK(writer.size() == 8);

    // part of it goes, and the rest of the buffer waits.
    held.make_room(5);
    REQUIRE_FALSE(writing.is_ready());

    // the rest of the buffer, then part of what's left straight from it (as more than the buffer holds is left), then
    // the rest's buffered.
    held.make_room(9);
    REQUIRE(writing.is_ready());
    res = std::move(writing).result();
    CHECK_FALSE(res.err);
    CHECK(res.count == 20);
    CHECK(held.out == "ABCDEFGHIJKLMN"sv);
    CHECK(held.asked == std::vector<std::size_t>{8, 3, 14});
    CHECK(writer.size() == 8);

    // and the writer's free for its flush.
    auto flushing = test::started(writer.flush());
    REQUIRE_FALSE(flushing.is_ready());
    held.make_room(100);
    REQUIRE(flushing.is_ready());
    res = std::move(flushing).result();
    CHECK_FALSE(res.err);
    CHECK(res.count == 8);
    CHECK(held.out == "ABCDEFGHIJKLMNOPQRSTUV"sv);
    CHECK(writer.size() == 0);
}

TEST_CASE("a write of more than the buffer holds, whose writes wait, gives what it wrote along with its failure",
          "[io][buffered_writer][through]")
{
    using namespace std::string_view_literals;

    test::held_writer        held;
    net::io::buffered_writer writer{&held, 8};

    SECTION("the buffer's flush")
    {
        auto writing = test::started(writer.write("ABCDEFGHIJKLMNOPQRST"sv));
        REQUIRE_FALSE(writing.is_ready());

        held.fail(std::make_error_code(std::errc::broken_pipe));
        REQUIRE(writing.is_ready());
        const auto res = std::move(writing).result();
        CHECK(res.err == std::errc::broken_pipe);
        CHECK(res.count == 8);

        // what wasn't written is kept.
        CHECK(writer.size() == 8);
    }

    SECTION("a write straight from it")
    {
        auto writing = test::started(writer.write("ABCDEFGHIJKLMNOPQRST"sv));
        REQUIRE_FALSE(writing.is_ready());

        held.make_room(8);
        REQUIRE_FALSE(writing.is_ready());
        held.fail(std::make_error_code(std::errc::broken_pipe));
        REQUIRE(writing.is_ready());
        const auto res = std::move(writing).result();
        CHECK(res.err == std::errc::broken_pipe);
        CHECK(res.count == 8);
        CHECK(held.out == "ABCDEFGH"sv);
    }
}

TEST_CASE("a flush whose writes wait writes it all, and is for one at a time", "[io][buffered_writer][flush]")
{
    using namespace std::string_view_literals;

    test::held_writer        held;
    net::io::buffered_writer writer{&held, 8};

    auto res = test::sync_wait(writer.write("ABCDEF"sv));
    REQUIRE(res.count == 6);

    auto flushing = test::started(writer.flush());
    REQUIRE_FALSE(flushing.is_ready());

    // another flush, or a write that doesn't fit, while it waits is busy, writing nothing.
    auto other = writer.flush();
    REQUIRE(other.finished_without_coroutine());
    const auto other_res = std::move(other).result();
    CHECK(other_res.err == std::errc::device_or_resource_busy);
    auto through = writer.write("GHIJ"sv);
    REQUIRE(through.finished_without_coroutine());
    const auto through_res = std::move(through).result();
    CHECK(through_res.err == std::errc::device_or_resource_busy);

    held.make_room(2);
    REQUIRE_FALSE(flushing.is_ready());
    held.make_room(4);
    REQUIRE(flushing.is_ready());
    res = std::move(flushing).result();
    CHECK_FALSE(res.err);
    CHECK(res.count == 6);
    CHECK(held.out == "ABCDEF"sv);
    CHECK(held.asked == std::vector<std::size_t>{6, 4});
}
