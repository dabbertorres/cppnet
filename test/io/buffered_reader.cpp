#include "io/buffered_reader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/socket.h>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/string_reader.hpp"
#include "io/writer.hpp"
#include "tcp.hpp"

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

// write_then_raise writes text to `to`, then raises written.
net::coro::task<> write_then_raise(net::io::scheduler*         sched,
                                   net::tcp_socket*            to,
                                   std::string_view            text,
                                   std::atomic<std::uint64_t>* written)
{
    (void)co_await net::io::write_all(*to, text);
    written->store(1);
    sched->notify_all(*written);
}

// fill_while_writing awaits fill (made before, so that another could be tried meanwhile) while a task of its own writes
// text to `to` (so the text may come before or after the fill waits), then waits for that task too.
net::coro::task<std::error_code> fill_while_writing(net::io::scheduler*              sched,
                                                    net::coro::task<std::error_code> fill,
                                                    net::tcp_socket*                 to,
                                                    std::string_view                 text)
{
    std::atomic<std::uint64_t> written{0};
    if (!sched->spawn(write_then_raise(sched, to, text, &written)))
        co_return std::make_error_code(std::errc::operation_canceled);

    const auto err = co_await std::move(fill);
    while (written.load() == 0) (void)co_await sched->wait_on(written, 0);
    co_return err;
}

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

TEST_CASE("a fill that waits for a socket is finished when its data comes, and another meanwhile is busy",
          "[io][buffered_reader][fill]")
{
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);

    net::io::scheduler       sched{std::make_shared<net::coro::thread_pool>(2)};
    net::tcp_socket          reading{&sched, fds[0]};
    net::tcp_socket          writing{&sched, fds[1]};
    net::io::buffered_reader reader{&reading, 64};

    // nothing's there: the socket's read has to wait, so the fill is the adapter's.
    auto first = reader.fill();
    REQUIRE_FALSE(first.finished_without_coroutine());

    auto second = reader.fill();
    REQUIRE(second.finished_without_coroutine());
    const auto busy = std::move(second).result();
    CHECK(busy == std::errc::device_or_resource_busy);

    const auto err = sched.run_to_completion(fill_while_writing(&sched, std::move(first), &writing, "hello"sv));
    REQUIRE_FALSE(err);

    // what one read of the socket gave (the writer's one write, unless it came in pieces).
    const auto             got = reader.buffered();
    const std::string_view text{reinterpret_cast<const char*>(got.data()), got.size()};
    CHECK("hello"sv.starts_with(text));
    CHECK_FALSE(text.empty());
}
