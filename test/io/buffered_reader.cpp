#include "io/buffered_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/string_reader.hpp"

using namespace std::string_view_literals;

namespace
{

// stuck_reader breaks the reader contract by reading nothing, without reporting an error.
class stuck_reader : public net::io::reader
{
public:
    net::coro::task<net::io::result> read(std::span<std::byte> /*data*/) override { co_return {.count = 0}; }

    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }
};

// recording_reader reads from text as much as each read asks for, and records how much that was.
class recording_reader : public net::io::reader
{
public:
    explicit recording_reader(std::string_view text) noexcept
        : text{text}
    {}

    net::coro::task<net::io::result> read(std::span<std::byte> data) override
    {
        asked.push_back(data.size());
        const auto n = std::min(data.size(), text.size());
        std::ranges::copy(std::as_bytes(std::span{text.substr(0, n)}), data.begin());
        text.remove_prefix(n);
        return net::coro::ready_task<net::io::result>(net::io::result{.count = n});
    }

    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

    std::vector<std::size_t> asked;

private:
    std::string_view text;
};

}

TEST_CASE("buffer reads to capacity as needed", "[io][buffered_reader]")
{
    net::io::string_reader   string("foobarbaz"sv);
    net::io::buffered_reader reader(&string, 4);

    std::string buf(6, 0);

    auto res = test::sync_wait(reader.read(buf));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 6);
    REQUIRE(buf == "foobar"sv);

    auto [next, have_next] = test::sync_wait(reader.peek());
    REQUIRE(have_next);
    REQUIRE(static_cast<char>(next) == 'b');

    // (what's buffered, then the end, which comes with it, as the read is larger than the buffer.)
    res = test::sync_wait(reader.read(buf));
    CHECK(res.err == net::io::errc::end_of_stream);
    REQUIRE(res.count == 3);
    REQUIRE(buf.substr(0, res.count) == "baz"sv);

    auto [next_again, have_next_again] = test::sync_wait(reader.peek());
    REQUIRE_FALSE(have_next_again);
    REQUIRE(static_cast<char>(next_again) == 0);
}

TEST_CASE("0-length read", "[io][buffered_reader]")
{
    net::io::string_reader   string("foobarbaz"sv);
    net::io::buffered_reader reader(&string, 4);

    std::string buf;

    auto res = test::sync_wait(reader.read(buf));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 0);
}

TEST_CASE("large read past the end reports closed", "[io][buffered_reader]")
{
    net::io::string_reader   string("abc"sv);
    net::io::buffered_reader reader(&string, 4);

    // larger than the buffer's capacity, so this reads straight from the underlying reader.
    std::string buf(20, 0);

    auto res = test::sync_wait(reader.read(buf));
    CHECK(res.err == net::io::errc::end_of_stream);
    CHECK(res.count == 3);
    CHECK(buf.substr(0, res.count) == "abc"sv);
}

TEST_CASE("reader that makes no progress is an error", "[io][buffered_reader]")
{
    stuck_reader             stuck;
    net::io::buffered_reader reader(&stuck, 4);

    std::string large(20, 0);

    auto res = test::sync_wait(reader.read(large));
    CHECK(res.err == net::io::errc::no_progress);

    std::string small(2, 0);

    res = test::sync_wait(reader.read(small));
    CHECK(res.err == net::io::errc::no_progress);
}

TEST_CASE("large read goes past the buffer at once", "[io][buffered_reader]")
{
    recording_reader         underlying{"0123456789abcdef"sv};
    net::io::buffered_reader reader(&underlying, 4);

    // larger than the buffer: what's buffered first, then the rest in one read of the underlying reader.
    std::string buf(2, 0);
    auto        res = test::sync_wait(reader.read(buf));
    REQUIRE_FALSE(res.err);
    CHECK(buf == "01"sv);

    buf.assign(10, 0);
    res = test::sync_wait(reader.read(buf));
    REQUIRE_FALSE(res.err);
    REQUIRE(res.count == 10);
    CHECK(buf == "23456789ab"sv);
    CHECK(underlying.asked == std::vector<std::size_t>{4, 8});
}
