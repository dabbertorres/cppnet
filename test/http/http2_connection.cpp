#include "http/http2_connection.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
#include "http/http2_session.hpp"
#include "http/priority.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace http2  = net::http::http2;
namespace hpack  = net::http::hpack;
namespace detail = net::http::detail;

using net::coro::task;

namespace
{

// test_connection is an HTTP/2 connection that isn't run: the test feeds its events, as the reader would.
class test_connection final : public detail::http2_connection_base
{
public:
    test_connection(net::io::scheduler* scheduler, detail::connection& conn)
        : http2_connection_base{scheduler, conn, http2::role::server, {}, *spdlog::default_logger()}
    {}

    using http2_connection_base::add_stream;
    using http2_connection_base::end;
    using http2_connection_base::end_received;
    using http2_connection_base::on_data;
    using http2_connection_base::on_stream_reset;
    using http2_connection_base::remove_stream;
    using http2_connection_base::run;

protected:
    void on_headers(http2::stream_id /*id*/,
                    http2::block_kind /*kind*/,
                    std::span<const net::http::hpack::field> /*fields*/,
                    bool /*end_stream*/) override
    {}
    void on_header_list_too_large(http2::stream_id /*id*/, bool /*end_stream*/) override {}
    void on_goaway(http2::stream_id /*last*/, std::error_code /*err*/, std::string_view /*debug*/) override {}
    void on_ping_ack(std::span<const std::byte, 8> /*opaque*/) override {}
    void on_priority_update(http2::stream_id /*id*/, net::http::priority /*p*/) override {}
    void on_origin(std::span<const std::string_view> /*origins*/) override {}

    std::optional<net::io::clock::time_point> check_connection(net::io::clock::time_point /*now*/) override
    {
        return std::nullopt;
    }
};

// fed_stream is a stream of a test_connection, which the test feeds as the connection's reader would, and the stream's
// reader, on a scheduler of one worker: so a task the test starts with then() before a read that has to wait runs once
// the read waits, and the read's task goes on once that task's done (the read's woken on the pool). A watch fails the
// stream after 10 s, so a read that's never woken fails, rather than hangs.
class fed_stream
{
public:
    fed_stream(net::io::scheduler* sched, detail::connection& conn)
        : sched{sched}
        , c{sched, conn}
    {
        {
            std::scoped_lock lock{c.mu};
            c.add_stream(stream);
        }
        (void)sched->spawn(watch(sched, this, stop_watch.get_token()));
    }

    fed_stream(const fed_stream&)            = delete;
    fed_stream& operator=(const fed_stream&) = delete;
    fed_stream(fed_stream&&)                 = delete;
    fed_stream& operator=(fed_stream&&)      = delete;

    ~fed_stream()
    {
        stop_watch.request_stop();

        // a tunnel's stop callback takes the lock, under which the tunnel goes.
        if (stream.tunnel != nullptr) stream.tunnel->set_stop_token({});
        std::scoped_lock lock{c.mu};
        c.remove_stream(stream.id);
        stream.tunnel.reset();
    }

    // arrive is DATA for the stream.
    void arrive(std::string_view data, bool end_stream = false)
    {
        std::scoped_lock lock{c.mu};
        c.on_data(stream.id, std::as_bytes(std::span{data}), end_stream);
    }

    // free_chunks is how many free chunks the connection keeps.
    std::size_t free_chunks()
    {
        std::scoped_lock lock{c.mu};
        return c.chunks.size();
    }

    // then runs f on a task of its own: once the test's task waits, if it's about to.
    void then(std::function<void(fed_stream&)> f)
    {
        if (sched->spawn(run(this, std::move(f)))) ++spawned;
    }

    // settled waits until every task then() started has run (on the one worker, once the test's task waits here, if it
    // hasn't before): a test calls it before it returns, so that one left by a read that didn't wait never runs after
    // what it uses has gone.
    task<> settled()
    {
        for (auto seen = ran.load(); seen < spawned; seen = ran.load())
        {
            if (co_await sched->wait_on(ran, seen)) break;
        }
    }

    // make_tunnel makes the stream a tunnel (with no closing token), as its maker does.
    detail::stream_tunnel& make_tunnel()
    {
        std::scoped_lock lock{c.mu};
        stream.tunnel = std::make_unique<detail::stream_tunnel>(stream, std::stop_token{}, detail::tunnel_options{});
        return *stream.tunnel;
    }

    // waiting is whether a read waits for the stream.
    bool waiting()
    {
        std::scoped_lock lock{c.mu};
        return stream.reading != nullptr;
    }

    net::io::scheduler*       sched;
    test_connection           c;
    detail::http2_stream_base stream{&c, 1, sched->pool()};
    detail::stream_reader     reader{&stream};

private:
    // NOTE: self may be gone once ran's bumped (settled() returns), so it's not touched after.
    static task<> run(fed_stream* self, std::function<void(fed_stream&)> f)
    {
        f(*self);
        auto* sched = self->sched;
        auto& ran   = self->ran;
        ran.fetch_add(1);
        sched->notify_all(ran);
        co_return;
    }

    // NOTE: self may be gone once stop's requested, so its sleep's ended: it's not touched then.
    static task<> watch(net::io::scheduler* sched, fed_stream* self, std::stop_token stop)
    {
        const auto err = co_await sched->sleep_until(net::io::clock::now() + std::chrono::seconds{10}, std::move(stop));
        if (err) co_return;

        {
            std::scoped_lock lock{self->c.mu};
            self->c.fail_stream(self->stream, std::make_error_code(std::errc::timed_out));
        }
        self->c.stop_pending();
    }

    std::stop_source           stop_watch;
    std::uint64_t              spawned = 0; // tasks then() started (the test's task's alone)
    std::atomic<std::uint64_t> ran{0};      // and those that have run
};

// found is what a test's task found amiss, for the test thread to report (Catch2's macros are for the test thread).
struct found
{
    std::vector<std::string> failed;

    // expect notes what, unless ok.
    void expect(bool ok, std::string what)
    {
        if (!ok) failed.push_back(std::move(what));
    }
};

// pattern is n bytes that differ from their neighbours (as text), from seed on: so data that's out of order, or
// repeated, doesn't compare equal.
std::string pattern(std::size_t n, std::size_t seed = 0)
{
    std::string out(n, '\0');
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<char>('!' + ((seed + i) % 89));
    return out;
}

// read_text reads from reader until it's read n bytes, in reads of at most piece, returning what it read, or why a read
// failed.
task<std::expected<std::string, std::error_code>>
read_text(detail::stream_reader& reader, std::size_t n, std::size_t piece)
{
    std::string            out;
    std::vector<std::byte> buffer(piece);
    while (out.size() < n)
    {
        const auto res = co_await reader.read(std::span{buffer}.first(std::min(piece, n - out.size())));
        if (res.err) co_return std::unexpected(res.err);
        out.append(reinterpret_cast<const char*>(buffer.data()), res.count);
    }
    co_return out;
}

// text is what a read read into data.
std::string text(std::span<const std::byte> data, const net::io::result& res)
{
    const auto read = data.first(res.count);
    return {reinterpret_cast<const char*>(read.data()), read.size()};
}

// described is what a read read into data, or why it failed, for a test's findings.
std::string described(std::span<const std::byte> data, const net::io::result& res)
{
    if (res.err) return std::format("{} byte(s), then {}", res.count, res.err.message());
    return std::format("\"{}\"", text(data, res));
}

// read_into reads once into data, into res: for a read that's started, and destroyed, by hand.
task<> read_into(detail::stream_reader* reader, std::span<std::byte> data, net::io::result* res)
{
    *res = co_await reader->read(data);
}

// connected is a connection to a listener of its own, for a test_connection to hold (it's never read nor written).
task<std::expected<detail::connection, std::error_code>> connected(net::io::scheduler* sched, std::uint16_t port)
{
    auto socket = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    if (!socket.has_value()) co_return std::unexpected(socket.error());
    co_return detail::connection{std::move(*socket)};
}

}

TEST_CASE("an HTTP/2 stream holds no more chunks than what it hasn't read needs, when its reader never catches up",
          "[http][http2][connection][flow][chunks]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        fed_stream f{sched, *conn};

        // a window's worth arrives; then, again and again, half of it is read (a little at a time) and as much more
        // arrives, so there's always some left to read: 2 MiB in all.
        constexpr std::size_t half = 65'535 / 2;
        std::size_t           seed = 0;
        f.arrive(pattern(half, seed));
        f.arrive(pattern(half, seed + half));
        for (int round = 0; round < 64; ++round)
        {
            const auto read = co_await read_text(f.reader, half, 1'000);
            if (read != pattern(half, seed))
            {
                out.expect(false, std::format("round {}: not what arrived", round));
                co_return out;
            }
            seed += half;
            f.arrive(pattern(half, seed + half));
        }
        auto read = co_await read_text(f.reader, half, 1'000);
        out.expect(read == pattern(half, seed), "the last but one half: not what arrived");
        read = co_await read_text(f.reader, half, 1'000);
        out.expect(read == pattern(half, seed + half), "the last half: not what arrived");

        // the chunks it took came back as they were read, and were taken again: as many as were ever unread at once
        // needed (a window's worth, over 5 chunks at most), rather than one for each 16 KiB that arrived.
        const auto kept = f.free_chunks();
        out.expect(kept > 0 && kept <= 5, std::format("{} chunk(s) kept, not 1 to 5", kept));
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, listener.local_port()));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 stream's read that waits takes DATA straight into its span, and what doesn't fit next",
          "[http][http2][connection][read]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        fed_stream f{sched, *conn};

        std::array<std::byte, 8> data{};

        // what fits goes into the span, and none of it to what the stream holds: two frames, as they come together.
        f.then(
            [](fed_stream& f)
            {
                f.arrive("abc");
                f.arrive("de");
            });
        auto res = co_await f.reader.read(data);
        out.expect(!res.err && text(data, res) == "abcde", "the first read: " + described(data, res));
        {
            // (no chunk was ever taken.)
            std::scoped_lock lock{f.c.mu};
            out.expect(f.stream.received.empty(), "the stream held some of it");
            out.expect(f.c.chunks.size() == 0, "a chunk was taken");
        }

        // what doesn't fit is the next read's, in order, even what comes after the span's full.
        f.then(
            [](fed_stream& f)
            {
                f.arrive("0123456");
                f.arrive("789");
                f.arrive("ab");
            });
        res = co_await f.reader.read(data);
        out.expect(!res.err && text(data, res) == "01234567", "the second read: " + described(data, res));

        res = co_await f.reader.read(data);
        out.expect(!res.err && text(data, res) == "89ab", "the third read: " + described(data, res));
        out.expect(!f.waiting(), "a read still waits");

        co_await f.settled();
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, listener.local_port()));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 stream's read destroyed while it waits leaves what comes for the next",
          "[http][http2][connection][read]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        fed_stream f{sched, *conn};

        // a read that waits, into a buffer that goes with it (so writing to it after is a use after free).
        auto            gone = std::make_unique<std::array<std::byte, 8>>();
        net::io::result res;
        auto            reading = read_into(&f.reader, *gone, &res);
        if (!reading.resume() || !f.waiting())
        {
            out.expect(false, "the read didn't wait");
            co_return out;
        }

        reading = {};
        gone.reset();
        out.expect(!f.waiting(), "the read destroyed still waits");

        f.arrive("xyz");
        std::array<std::byte, 8> data{};
        res = co_await f.reader.read(data);
        out.expect(!res.err && text(data, res) == "xyz", "the next read: " + described(data, res));
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, listener.local_port()));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 stream's read that waits ends with the stream, its trailers handed over as it returns",
          "[http][http2][connection][read][trailers]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port, bool trailing) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        fed_stream f{sched, *conn};

        net::http::trailer_fields kept;
        {
            std::scoped_lock lock{f.c.mu};
            f.stream.trailers_to = &kept;
        }

        std::array<std::byte, 8> data{};
        f.arrive("abc");
        auto res = co_await f.reader.read(data);
        out.expect(!res.err && text(data, res) == "abc", "the first read: " + described(data, res));

        // the end comes while the next read waits: END_STREAM on DATA, or trailers, which stay the connection's
        // until the read returns.
        bool kept_before = false;
        f.then(
            [trailing, &kept, &kept_before](fed_stream& f)
            {
                if (!trailing)
                {
                    f.arrive("", true);
                    return;
                }

                std::scoped_lock lock{f.c.mu};
                f.stream.received_trailers = std::make_unique<net::http::trailer_fields>();
                f.stream.received_trailers->add("Grpc-Status", "0");
                test_connection::end_received(f.stream);
                kept_before = !kept.empty();
            });
        res = co_await f.reader.read(data);
        co_await f.settled();
        out.expect(res.count == 0 && res.err == net::io::errc::end_of_stream, "the end: " + described(data, res));
        out.expect(!kept_before, "the trailers were handed over before the read returned");
        if (trailing) out.expect(kept.get("Grpc-Status") == "0", "the trailers weren't handed over");
        else out.expect(kept.empty(), "trailers were handed over");

        // and the next read finds the end at once.
        res = co_await f.reader.read(data);
        out.expect(res.err == net::io::errc::end_of_stream, "the next read: " + described(data, res));
        co_return out;
    };

    const bool trailing = GENERATE(false, true);
    CAPTURE(trailing);
    const auto out = sched.run_to_completion(test(&sched, listener.local_port(), trailing));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 stream's read that waits fails with its stream", "[http][http2][connection][read]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        fed_stream f{sched, *conn};

        std::array<std::byte, 8> data{};
        f.then(
            [](fed_stream& f)
            {
                std::scoped_lock lock{f.c.mu};
                f.c.on_stream_reset(f.stream.id, make_error_code(http2::error_code::cancel));
            });
        const auto res = co_await f.reader.read(data);
        out.expect(res.count == 0 && res.err == http2::error_code::cancel, "the read: " + described(data, res));
        out.expect(!f.waiting(), "a read still waits");

        co_await f.settled();
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, listener.local_port()));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 tunnel's read that waits fails, having read nothing, when it's stopped or its deadline's now",
          "[http][http2][connection][read][tunnel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port, bool stopping) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        fed_stream f{sched, *conn};
        auto&      tunnel = f.make_tunnel();

        std::stop_source stop;
        tunnel.set_stop_token(stop.get_token());

        // through the tunnel's buffer, whose fill waits.
        std::array<std::byte, 8> data{};
        f.then(
            [stopping, &stop, &tunnel](fed_stream& /*f*/)
            {
                if (stopping) stop.request_stop();
                else tunnel.set_read_deadline(net::io::clock::now());
            });
        auto res = co_await tunnel.reader().read(data);
        co_await f.settled();
        const auto want = stopping ? std::errc::operation_canceled : std::errc::timed_out;
        out.expect(res.count == 0 && res.err == want, "the read: " + described(data, res));
        out.expect(!f.waiting(), "a read still waits");

        // the stream goes on: what comes next is read.
        tunnel.set_stop_token({});
        tunnel.set_read_deadline(std::nullopt);
        f.then([](fed_stream& f) { f.arrive("next"); });
        res = co_await tunnel.reader().read(data);
        out.expect(!res.err && text(data, res) == "next", "the next read: " + described(data, res));

        co_await f.settled();
        co_return out;
    };

    const bool stopping = GENERATE(true, false);
    CAPTURE(stopping);
    const auto out = sched.run_to_completion(test(&sched, listener.local_port(), stopping));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 stream has one read waiting at a time", "[http][http2][connection][read]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        fed_stream f{sched, *conn};

        // another read of the same reader, or of another reader of the stream (as a tunnel's is, beside its request's
        // body's), while one waits: each returns at once (rather than waiting too).
        std::array<std::byte, 8> data{};
        net::io::result          same;
        net::io::result          other;
        bool                     same_waited  = false;
        bool                     other_waited = false;
        detail::stream_reader    another{&f.stream};
        f.then(
            [&](fed_stream& f)
            {
                auto first   = read_into(&f.reader, data, &same);
                auto second  = read_into(&another, data, &other);
                same_waited  = first.resume();
                other_waited = second.resume();
                f.arrive("go");
            });
        const auto res = co_await f.reader.read(data);
        co_await f.settled();
        out.expect(!same_waited && !other_waited, "another read waited");
        out.expect(!res.err && text(data, res) == "go", "the read: " + described(data, res));
        out.expect(same.err == std::errc::device_or_resource_busy, "the same reader's: " + same.err.message());
        out.expect(other.err == std::errc::device_or_resource_busy, "another reader's: " + other.err.message());
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, listener.local_port()));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 connection's chunk pool keeps chunks given back, up to its limit",
          "[http][http2][connection][chunks]")
{
    detail::chunk_pool pool;

    // none kept: new ones.
    auto* a = pool.take();
    auto* b = pool.take();
    REQUIRE(a != b);
    CHECK(pool.size() == 0);

    // kept, and taken again (the last given first).
    pool.give(a);
    pool.give(b);
    CHECK(pool.size() == 2);
    auto* again = pool.take();
    CHECK(again == b);
    pool.give(again);
    CHECK(pool.size() == 2);

    // past the limit, what's given back is freed (as is what's kept past a lower limit).
    constexpr auto                       limit = detail::chunk_pool::limit_for(0);
    std::vector<detail::received_chunk*> taken;
    taken.reserve(limit + 3);
    for (std::size_t i = 0; i < limit + 3; ++i) taken.push_back(pool.take());
    for (auto* chunk : taken) pool.give(chunk);
    CHECK(pool.size() == limit);

    pool.set_limit(2);
    CHECK(pool.size() == 2);
    pool.set_limit(limit);
    CHECK(pool.size() == 2);

    // (what it keeps when it's destroyed, it frees.)
}

TEST_CASE("an HTTP/2 connection's chunk pool trims the chunks none took since the last trim",
          "[http][http2][connection][chunks]")
{
    detail::chunk_pool pool;

    std::array<detail::received_chunk*, 5> taken{};
    for (auto& chunk : taken) chunk = pool.take();
    for (auto* chunk : taken) pool.give(chunk);

    // none were kept at the first trim's start: nothing goes.
    CHECK(pool.trim());
    CHECK(pool.size() == 5);

    // traffic takes 2 at once, and gives them back: the 3 it never took go.
    auto* x = pool.take();
    auto* y = pool.take();
    pool.give(x);
    pool.give(y);
    CHECK(pool.trim());
    CHECK(pool.size() == 2);

    // as long as traffic takes them, they stay.
    pool.give(pool.take());
    CHECK(pool.trim());
    CHECK(pool.size() == 1);

    // and once it stops, the rest go.
    CHECK_FALSE(pool.trim());
    CHECK(pool.size() == 0);
}

TEST_CASE("what an HTTP/2 stream receives spans chunks, as do its reads", "[http][http2][connection][chunks]")
{
    detail::chunk_pool    pool;
    detail::received_data data;
    constexpr auto        chunk = detail::received_chunk::capacity;

    const auto append = [&](std::string_view text) { data.append(std::as_bytes(std::span{text}), pool); };
    const auto take   = [&](std::size_t n)
    {
        std::string out(n, '\0');
        out.resize(data.take(std::as_writable_bytes(std::span{out}), pool));
        return out;
    };

    // a frame larger than a chunk, then one that ends past the end of the chunk it starts in, then one in the next.
    const auto all = pattern((2 * chunk) + 3'000 + 15'000 + 7);
    append(std::string_view{all}.substr(0, (2 * chunk) + 3'000));
    append(std::string_view{all}.substr((2 * chunk) + 3'000, 15'000));
    append(std::string_view{all}.substr((2 * chunk) + 18'000));
    CHECK(data.size() == all.size());
    CHECK(pool.size() == 0);

    // nothing taken by an empty span; then reads in pieces that end inside chunks and past their ends, each chunk
    // given back once it's read to its end.
    CHECK(take(0).empty());
    std::string read = take(5'000);
    CHECK(pool.size() == 0);
    read += take(chunk);
    CHECK(pool.size() == 1);
    read += take(chunk + 100);
    CHECK(pool.size() == 2);
    read += take(all.size()); // more than's left
    CHECK(read == all);
    CHECK(data.empty());
    CHECK(pool.size() == 4);

    // what arrives next takes the chunks given back (nothing new), and so on.
    append(std::string_view{all}.substr(0, chunk + 1));
    CHECK(pool.size() == 2);
    CHECK(take(all.size()) == all.substr(0, chunk + 1));
    CHECK(pool.size() == 4);

    // nothing's taken for nothing; and clearing gives back what's unread.
    append({});
    CHECK(pool.size() == 4);
    append(std::string_view{all}.substr(0, 3 * chunk));
    CHECK(pool.size() == 1);
    CHECK(take(10) == all.substr(0, 10));
    data.clear(pool);
    CHECK(data.empty());
    CHECK(pool.size() == 4);
    CHECK(take(10).empty());

    // and what it holds when it's destroyed, it frees (not back to the pool, which may be gone).
    append(std::string_view{all}.substr(0, 3'000));
}

TEST_CASE("an HTTP/2 stream's reads take what it received across chunks, giving them back for its next DATA",
          "[http][http2][connection][read][chunks]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        fed_stream f{sched, *conn};

        // three frames of 10 KB, read 7 KB at a time.
        const auto all = pattern(30'000);
        for (std::size_t i = 0; i < all.size(); i += 10'000) f.arrive(std::string_view{all}.substr(i, 10'000));
        auto read = co_await read_text(f.reader, all.size(), 7'000);
        out.expect(read == all, "the first reads: not what arrived");
        out.expect(f.free_chunks() == 2, std::format("{} chunk(s) kept after the first reads, not 2", f.free_chunks()));

        // the next takes them again.
        f.arrive(all);
        out.expect(f.free_chunks() == 0, std::format("{} chunk(s) kept as more arrived, not 0", f.free_chunks()));
        read = co_await read_text(f.reader, all.size(), 30'000);
        out.expect(read == all, "the next read: not what arrived");
        out.expect(f.free_chunks() == 2, std::format("{} chunk(s) kept after the next read, not 2", f.free_chunks()));
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, listener.local_port()));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 stream's read that waits takes what's received in chunks before it, then what comes",
          "[http][http2][connection][read][chunks]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        fed_stream f{sched, *conn};

        // what doesn't fit in the waiting read's span goes to chunks, and so does what comes after it (in order, though
        // the read hasn't taken its result yet), spanning them.
        const auto             all = pattern(40'000);
        std::vector<std::byte> data(8'000);
        f.then(
            [&all](fed_stream& f)
            {
                f.arrive(std::string_view{all}.substr(0, 20'000));
                f.arrive(std::string_view{all}.substr(20'000, 10'000));
            });
        auto res = co_await f.reader.read(data);
        out.expect(!res.err && text(data, res) == all.substr(0, 8'000), "the first read: not what arrived");

        // a read finds them at once; then the next waits again, and what comes goes straight into its span.
        const auto read = co_await read_text(f.reader, 22'000, 22'000);
        out.expect(read == all.substr(8'000, 22'000), "the second read: not what arrived");
        out.expect(f.free_chunks() == 2, std::format("{} chunk(s) kept after the second read, not 2", f.free_chunks()));

        f.then([&all](fed_stream& f) { f.arrive(std::string_view{all}.substr(30'000)); });
        res = co_await f.reader.read(data);
        out.expect(!res.err && text(data, res) == all.substr(30'000, 8'000), "the third read: not what arrived");
        // (the rest, 2 KB, in one of them.)
        out.expect(f.free_chunks() == 1, std::format("{} chunk(s) kept after the third read, not 1", f.free_chunks()));

        res = co_await f.reader.read(data);
        out.expect(!res.err && text(data, res) == all.substr(38'000), "the last read: not what arrived");
        out.expect(f.free_chunks() == 2, std::format("{} chunk(s) kept after the last read, not 2", f.free_chunks()));

        co_await f.settled();
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, listener.local_port()));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 stream reads its end after what it received in chunks", "[http][http2][connection][read][chunks]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port, bool trailing) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        fed_stream f{sched, *conn};

        net::http::trailer_fields kept;
        {
            std::scoped_lock lock{f.c.mu};
            f.stream.trailers_to = &kept;
        }

        // END_STREAM on the last DATA, or trailers after it.
        const auto all = pattern(40'000);
        f.arrive(std::string_view{all}.substr(0, 20'000));
        f.arrive(std::string_view{all}.substr(20'000), !trailing);
        if (trailing)
        {
            std::scoped_lock lock{f.c.mu};
            f.stream.received_trailers = std::make_unique<net::http::trailer_fields>();
            f.stream.received_trailers->add("Grpc-Status", "0");
            test_connection::end_received(f.stream);
        }

        const auto read = co_await read_text(f.reader, all.size(), 9'000);
        out.expect(read == all, "the reads: not what arrived");
        out.expect(kept.empty(), "the trailers were handed over before the end was read");

        std::array<std::byte, 8> data{};
        const auto               res = co_await f.reader.read(data);
        out.expect(res.count == 0 && res.err == net::io::errc::end_of_stream, "the end: " + described(data, res));
        if (trailing) out.expect(kept.get("Grpc-Status") == "0", "the trailers weren't handed over");
        out.expect(f.free_chunks() == 3, std::format("{} chunk(s) kept, not 3", f.free_chunks()));
        co_return out;
    };

    const bool trailing = GENERATE(false, true);
    CAPTURE(trailing);
    const auto out = sched.run_to_completion(test(&sched, listener.local_port(), trailing));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 stream's unread chunks go back to its connection when it's removed, past its limit freed",
          "[http][http2][connection][chunks]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }

        constexpr auto chunk = detail::received_chunk::capacity;
        const auto     limit = detail::chunk_pool::limit_for(1);

        test_connection           c{sched, *conn};
        detail::http2_stream_base stream{&c, 1, sched->pool()};
        const auto                arrive = [&](std::size_t n)
        {
            const auto       data = pattern(n);
            std::scoped_lock lock{c.mu};
            c.on_data(stream.id, std::as_bytes(std::span{data}), false);
        };
        const auto kept = [&]
        {
            std::scoped_lock lock{c.mu};
            return c.chunks.size();
        };

        {
            std::scoped_lock lock{c.mu};
            c.add_stream(stream);
        }

        // more chunks than the connection keeps for a stream, unread: all of them are the stream's.
        arrive((limit + 3) * chunk);
        out.expect(kept() == 0, std::format("{} chunk(s) kept while the stream held them, not 0", kept()));

        // removed, it gives them back: the connection keeps what its limit (now for no stream) allows.
        {
            std::scoped_lock lock{c.mu};
            c.remove_stream(stream.id);
            out.expect(stream.received.empty(), "the stream removed still holds what it received");
        }
        out.expect(
            kept() == detail::chunk_pool::limit_for(0),
            std::format("{} chunk(s) kept once it was removed, not {}", kept(), detail::chunk_pool::limit_for(0)));

        // a stream that's read them all gives back what its limit allows.
        detail::http2_stream_base other{&c, 3, sched->pool()};
        detail::stream_reader     reader{&other};
        {
            std::scoped_lock lock{c.mu};
            c.add_stream(other);
        }
        {
            const auto       data = pattern((limit + 3) * chunk);
            std::scoped_lock lock{c.mu};
            c.on_data(other.id, std::as_bytes(std::span{data}), false);
        }
        out.expect(kept() == 0, std::format("{} chunk(s) kept while the other held them, not 0", kept()));
        const auto read = co_await read_text(reader, (limit + 3) * chunk, std::size_t{64} * 1'024);
        out.expect(read.has_value() && read->size() == (limit + 3) * chunk, "the other's reads: not all of it");
        out.expect(kept() == limit, std::format("{} chunk(s) kept once it was read, not {}", kept(), limit));

        std::scoped_lock lock{c.mu};
        c.remove_stream(other.id);
        out.expect(c.chunks.size() == detail::chunk_pool::limit_for(0),
                   std::format("{} chunk(s) kept once the other was removed, not {}",
                               c.chunks.size(),
                               detail::chunk_pool::limit_for(0)));
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, listener.local_port()));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 stream that's destroyed with unread chunks frees them, as does its connection what it keeps",
          "[http][http2][connection][chunks]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    // (LeakSanitizer, in the tests' builds that have it, would find a chunk that isn't freed.)
    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }

        // as a client's connection ends with streams its responses' bodies still hold: they aren't removed.
        test_connection c{sched, *conn};
        {
            const auto       data = pattern(40'000);
            std::scoped_lock lock{c.mu};
            c.on_data(1, std::as_bytes(std::span{data}), false); // no such stream: nothing kept
            out.expect(c.chunks.size() == 0, "chunks were taken for a stream there isn't");
        }

        auto stream = std::make_unique<detail::http2_stream_base>(&c, 1, sched->pool());
        auto kept   = std::make_unique<detail::http2_stream_base>(&c, 3, sched->pool());
        {
            const auto       data = pattern(40'000);
            std::scoped_lock lock{c.mu};
            c.add_stream(*stream);
            c.add_stream(*kept);
            c.on_data(1, std::as_bytes(std::span{data}), false);
            c.on_data(3, std::as_bytes(std::span{data}), false);

            // one's removed, its chunks kept by the connection; the other's not.
            c.remove_stream(3);
            out.expect(c.chunks.size() == 3, std::format("{} chunk(s) kept, not 3", c.chunks.size()));
            out.expect(stream->received.size() == 40'000,
                       std::format("the other holds {} byte(s), not 40000", stream->received.size()));
        }
        kept.reset();
        stream.reset();
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, listener.local_port()));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 stream reads what it received in chunks before its connection ended, then the end",
          "[http][http2][connection][read][chunks]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, port);
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        fed_stream f{sched, *conn};

        const auto all = pattern(20'000);
        f.arrive(all);
        {
            std::scoped_lock lock{f.c.mu};
            f.c.end();
        }
        f.c.stop_pending();

        std::vector<std::byte> data(30'000);
        auto                   res = co_await f.reader.read(data);
        out.expect(!res.err && text(data, res) == all, "the read: not what arrived");
        out.expect(f.free_chunks() == 2, std::format("{} chunk(s) kept, not 2", f.free_chunks()));

        res = co_await f.reader.read(data);
        out.expect(res.count == 0 && res.err == std::errc::connection_aborted, "the end: " + described(data, res));
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, listener.local_port()));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("an HTTP/2 connection's timer frees the chunks none have taken for a while",
          "[http][http2][connection][chunks]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, net::listener* listener) -> task<found>
    {
        found out;
        auto  conn = co_await connected(sched, listener->local_port());
        if (!conn.has_value())
        {
            out.expect(false, "not connected: " + conn.error().message());
            co_return out;
        }
        auto peer = co_await listener->accept();
        if (!peer.has_value())
        {
            out.expect(false, "not accepted: " + peer.error().message());
            co_return out;
        }

        fed_stream f{sched, *conn};

        // the connection runs (its timer too), until its peer closes; the test waits for it to have returned.
        std::atomic<std::uint64_t> ran{0};
        const auto                 running = [](test_connection* c, std::atomic<std::uint64_t>* ran) -> task<>
        {
            co_await c->run({});
            auto* sched = c->get_scheduler();
            ran->store(1);
            sched->notify_all(*ran);
        };
        if (!sched->spawn(running(&f.c, &ran)))
        {
            out.expect(false, "the connection didn't run");
            co_return out;
        }

        const auto all = pattern(40'000);
        f.arrive(all);
        const auto read  = co_await read_text(f.reader, all.size(), all.size());
        const auto given = net::io::clock::now();
        out.expect(read == all, "the read: not what arrived");
        out.expect(f.free_chunks() == 3, std::format("{} chunk(s) kept once read, not 3", f.free_chunks()));

        // none take them: they're freed (at the second trim after they were given back), not before a trim's interval.
        const auto deadline = given + std::chrono::seconds{10};
        while (f.free_chunks() > 0 && net::io::clock::now() < deadline)
            (void)co_await sched->sleep_until(net::io::clock::now() + std::chrono::milliseconds{50});
        out.expect(f.free_chunks() == 0, std::format("{} chunk(s) still kept", f.free_chunks()));
        out.expect(net::io::clock::now() - given >= std::chrono::seconds{1}, "freed before a trim's interval");

        peer->close();
        while (ran.load() == 0)
        {
            if (const auto err = co_await sched->wait_on(ran, 0, deadline); err) break;
        }
        if (ran.load() == 0)
        {
            out.expect(false, "the connection didn't return once its peer closed");

            // (it uses f, and ran: it's ended, and waited for, before they go.)
            {
                std::scoped_lock lock{f.c.mu};
                f.c.end();
            }
            f.c.stop_pending();
            while (ran.load() == 0)
            {
                if (co_await sched->wait_on(ran, 0)) break;
            }
        }
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, &listener));
    CAPTURE(out.failed);
    CHECK(out.failed.empty());
}

TEST_CASE("HTTP/2 sends neither connection-specific fields, nor those Connection names", "[http][http2][connection]")
{
    const net::http::headers from{
        {       "Connection", {"keep-alive, X-Hop"}},
        {            "X-Hop",                 {"1"}},
        {       "Keep-Alive",         {"timeout=5"}},
        {"Transfer-Encoding",           {"chunked"}},
        {          "Upgrade",               {"h2c"}},
        {     "Content-Type",        {"text/plain"}},
        {          "X-Other",            {"a", "b"}},
    };

    std::vector<std::string>  names;
    std::vector<hpack::field> fields;
    REQUIRE_FALSE(detail::append_fields(from, names, fields));

    std::vector<std::string> got;
    got.reserve(fields.size());
    for (const auto& f : fields) got.push_back(std::string{f.name} + ": " + std::string{f.value});
    CHECK(got == std::vector<std::string>{"content-type: text/plain", "x-other: a", "x-other: b"});
}

TEST_CASE("HTTP/2 sends only the fields that may be trailers as trailers", "[http][http2][connection][trailers]")
{
    const net::http::trailer_fields from{
        { "Server-Timing",   {"db;dur=53"}},
        {"Content-Length",           {"0"}},
        {      "If-Match",       {"\"a\""}},
        {          "Host", {"example.com"}},
        { "Authorization",      {"secret"}},
        {    "Keep-Alive",   {"timeout=5"}},
        {   "Grpc-Status",           {"0"}},
    };

    std::vector<std::string>  names;
    std::vector<hpack::field> fields;
    REQUIRE_FALSE(detail::append_fields(from, names, fields, detail::field_section::trailers));

    std::vector<std::string> got;
    got.reserve(fields.size());
    for (const auto& f : fields) got.push_back(std::string{f.name} + ": " + std::string{f.value});
    CHECK(got == std::vector<std::string>{"server-timing: db;dur=53", "grpc-status: 0"});

    // one it would drop is still checked, as HTTP/1.1's chunked_writer checks it.
    net::http::trailer_fields bad;
    bad.add("Grpc-Status", "0");
    bad.add("Content-Length", "1\r\n2");
    fields.clear();
    CHECK(detail::append_fields(bad, names, fields, detail::field_section::trailers) == std::errc::invalid_argument);
    CHECK(fields.empty());
}

TEST_CASE("HTTP/2 sends values without the whitespace around them", "[http][http2][connection]")
{
    net::http::headers from;
    from.add("X-Leading", " a");
    from.add("X-Trailing", "a\t ");
    from.add("X-Both", "\t a b \t");
    from.add("X-Inside", "a \t b");
    from.add("X-Blank", " \t ");
    from.add("TE", " trailers ");

    std::vector<std::string>  names;
    std::vector<hpack::field> fields;
    REQUIRE_FALSE(detail::append_fields(from, names, fields, detail::field_section::request));

    std::vector<std::string> got;
    got.reserve(fields.size());
    for (const auto& f : fields) got.push_back(std::format("{}=[{}]", f.name, f.value));
    CHECK(got
          == std::vector<std::string>{"x-leading=[a]",
                                      "x-trailing=[a]",
                                      "x-both=[a b]",
                                      "x-inside=[a \t b]",
                                      "x-blank=[]",
                                      "te=[trailers]"});

    CHECK(detail::trim_value("").empty());
    CHECK(detail::trim_value("a") == "a");
    CHECK(detail::trim_value(" a b ") == "a b");
}

TEST_CASE("HTTP/2 doesn't send fields that HTTP/1.1 couldn't", "[http][http2][connection]")
{
    struct test_case
    {
        std::string_view name;
        std::string_view value;
    };

    const auto tc = GENERATE(test_case{.name = "X-Bad", .value = "a\r\nb"},
                             test_case{.name = "X-Bad", .value = "a\x01b"},
                             test_case{.name = "Bad Name", .value = "a"},
                             test_case{.name = "", .value = "a"});
    CAPTURE(tc.name, tc.value);

    net::http::headers from;
    from.set("Content-Type", "text/plain");
    from.set(tc.name, tc.value);

    std::vector<std::string>  names;
    std::vector<hpack::field> fields{
        {.name = ":status", .value = "200"}
    };
    CHECK(detail::append_fields(from, names, fields) == std::errc::invalid_argument);
    CHECK(fields.size() == 1);
}
