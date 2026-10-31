#include "http/http2_connection.hpp"

#include <algorithm>
#include <array>
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
    using http2_connection_base::end_received;
    using http2_connection_base::on_data;
    using http2_connection_base::on_stream_reset;
    using http2_connection_base::remove_stream;

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

    // then runs f on a task of its own: once the test's task waits, if it's about to.
    void then(std::function<void(fed_stream&)> f) { (void)sched->spawn(run(this, std::move(f))); }

    // make_tunnel makes the stream a tunnel (with no closing token), as its maker does.
    detail::stream_tunnel& make_tunnel()
    {
        std::scoped_lock lock{c.mu};
        stream.tunnel = std::make_unique<detail::stream_tunnel>(stream, std::stop_token{});
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
    static task<> run(fed_stream* self, std::function<void(fed_stream&)> f)
    {
        f(*self);
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

    std::stop_source stop_watch;
};

// text is what a read read into data.
std::string text(std::span<const std::byte> data, const net::io::result& res)
{
    const auto read = data.first(res.count);
    return {reinterpret_cast<const char*>(read.data()), read.size()};
}

// read_into reads once into data, into res: for a read that's started, and destroyed, by hand.
task<> read_into(detail::stream_reader* reader, std::span<std::byte> data, net::io::result* res)
{
    *res = co_await reader->read(data);
}

// connected is a connection to a listener of its own, for a test_connection to hold (it's never read nor written).
task<detail::connection> connected(net::io::scheduler* sched, std::uint16_t port)
{
    auto socket = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    REQUIRE(socket.has_value());
    co_return detail::connection{std::move(*socket)};
}

}

TEST_CASE("an HTTP/2 stream's buffer stays bounded when its reader never catches up", "[http][http2][connection][flow]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::string port) -> task<std::size_t>
    {
        // a connection for the base to hold (it's never read nor written).
        auto socket = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        REQUIRE(socket.has_value());
        detail::connection conn{std::move(*socket)};

        test_connection           c{sched, conn};
        detail::http2_stream_base stream{&c, 1, sched->pool()};
        detail::stream_reader     reader{&stream};

        constexpr std::size_t        window = 65'535;
        const std::vector<std::byte> chunk(window / 2, std::byte{'x'});
        const auto                   arrive = [&](std::span<const std::byte> data)
        {
            std::scoped_lock lock{c.mu};
            c.on_data(stream.id, data, false);
        };

        {
            std::scoped_lock lock{c.mu};
            c.add_stream(stream);
        }

        // a window's worth arrives; then, again and again, half of it is read (a little at a time) and as much more
        // arrives, so there's always some left to read.
        arrive(chunk);
        arrive(chunk);

        std::size_t            held = 0;
        std::vector<std::byte> buffer(1'024);
        for (int round = 0; round < 64; ++round)
        {
            for (std::size_t read = 0; read < chunk.size();)
            {
                const auto res =
                    co_await reader.read(std::span{buffer}.first(std::min(buffer.size(), chunk.size() - read)));
                REQUIRE_FALSE(res.err);
                read += res.count;
            }
            arrive(chunk);

            std::scoped_lock lock{c.mu};
            held = std::max(held, stream.received.size());
        }

        {
            std::scoped_lock lock{c.mu};
            c.remove_stream(stream.id);
        }
        co_return held;
    };

    // what's been read is dropped once it's half the buffer, so it holds at most about twice the window (rather than
    // all that's arrived: 2 MiB).
    CHECK(sched.run_to_completion(test(&sched, std::to_string(listener.local_port()))) <= std::size_t{2} * 65'535);
}

TEST_CASE("an HTTP/2 stream's read that waits takes DATA straight into its span, and what doesn't fit next",
          "[http][http2][connection][read]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<>
    {
        auto       conn = co_await connected(sched, port);
        fed_stream f{sched, conn};

        std::array<std::byte, 8> data{};

        // what fits goes into the span, and none of it to what the stream holds: two frames, as they come together.
        f.then(
            [](fed_stream& f)
            {
                f.arrive("abc");
                f.arrive("de");
            });
        auto res = co_await f.reader.read(data);
        REQUIRE_FALSE(res.err);
        CHECK(text(data, res) == "abcde");
        {
            std::scoped_lock lock{f.c.mu};
            CHECK(f.stream.received.capacity() == 0);
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
        REQUIRE_FALSE(res.err);
        CHECK(text(data, res) == "01234567");

        res = co_await f.reader.read(data);
        REQUIRE_FALSE(res.err);
        CHECK(text(data, res) == "89ab");
        CHECK_FALSE(f.waiting());
    };
    sched.run_to_completion(test(&sched, listener.local_port()));
}

TEST_CASE("an HTTP/2 stream's read destroyed while it waits leaves what comes for the next",
          "[http][http2][connection][read]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<>
    {
        auto       conn = co_await connected(sched, port);
        fed_stream f{sched, conn};

        // a read that waits, into a buffer that goes with it (so writing to it after is a use after free).
        auto            gone = std::make_unique<std::array<std::byte, 8>>();
        net::io::result res;
        auto            reading = read_into(&f.reader, *gone, &res);
        REQUIRE(reading.resume());
        REQUIRE(f.waiting());

        reading = {};
        gone.reset();
        CHECK_FALSE(f.waiting());

        f.arrive("xyz");
        std::array<std::byte, 8> data{};
        res = co_await f.reader.read(data);
        REQUIRE_FALSE(res.err);
        CHECK(text(data, res) == "xyz");
    };
    sched.run_to_completion(test(&sched, listener.local_port()));
}

TEST_CASE("an HTTP/2 stream's read that waits ends with the stream, its trailers handed over as it returns",
          "[http][http2][connection][read][trailers]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port, bool trailing) -> task<>
    {
        auto       conn = co_await connected(sched, port);
        fed_stream f{sched, conn};

        net::http::trailer_fields kept;
        {
            std::scoped_lock lock{f.c.mu};
            f.stream.trailers_to = &kept;
        }

        std::array<std::byte, 8> data{};
        f.arrive("abc");
        auto res = co_await f.reader.read(data);
        REQUIRE(text(data, res) == "abc");

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
        CHECK(res.count == 0);
        CHECK(res.err == net::io::errc::end_of_stream);
        CHECK_FALSE(kept_before);
        if (trailing) CHECK(kept.get("Grpc-Status") == "0");
        else CHECK(kept.empty());

        // and the next read finds the end at once.
        res = co_await f.reader.read(data);
        CHECK(res.err == net::io::errc::end_of_stream);
    };

    const bool trailing = GENERATE(false, true);
    CAPTURE(trailing);
    sched.run_to_completion(test(&sched, listener.local_port(), trailing));
}

TEST_CASE("an HTTP/2 stream's read that waits fails with its stream", "[http][http2][connection][read]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<>
    {
        auto       conn = co_await connected(sched, port);
        fed_stream f{sched, conn};

        std::array<std::byte, 8> data{};
        f.then(
            [](fed_stream& f)
            {
                std::scoped_lock lock{f.c.mu};
                f.c.on_stream_reset(f.stream.id, make_error_code(http2::error_code::cancel));
            });
        const auto res = co_await f.reader.read(data);
        CHECK(res.count == 0);
        CHECK(res.err == http2::error_code::cancel);
        CHECK_FALSE(f.waiting());
    };
    sched.run_to_completion(test(&sched, listener.local_port()));
}

TEST_CASE("an HTTP/2 tunnel's read that waits fails, having read nothing, when it's stopped or its deadline's now",
          "[http][http2][connection][read][tunnel]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port, bool stopping) -> task<>
    {
        auto       conn = co_await connected(sched, port);
        fed_stream f{sched, conn};
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
        CHECK(res.count == 0);
        if (stopping) CHECK(res.err == std::errc::operation_canceled);
        else CHECK(res.err == std::errc::timed_out);
        CHECK_FALSE(f.waiting());

        // the stream goes on: what comes next is read.
        tunnel.set_stop_token({});
        tunnel.set_read_deadline(std::nullopt);
        f.then([](fed_stream& f) { f.arrive("next"); });
        res = co_await tunnel.reader().read(data);
        REQUIRE_FALSE(res.err);
        CHECK(text(data, res) == "next");
    };

    const bool stopping = GENERATE(true, false);
    CAPTURE(stopping);
    sched.run_to_completion(test(&sched, listener.local_port(), stopping));
}

TEST_CASE("an HTTP/2 stream has one read waiting at a time", "[http][http2][connection][read]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();

    const auto test = [](net::io::scheduler* sched, std::uint16_t port) -> task<>
    {
        auto       conn = co_await connected(sched, port);
        fed_stream f{sched, conn};

        // another read of the same reader, or of another reader of the stream (as a tunnel's is, beside its request's
        // body's), while one waits.
        std::array<std::byte, 8> data{};
        net::io::result          same;
        net::io::result          other;
        detail::stream_reader    another{&f.stream};
        f.then(
            [&](fed_stream& f)
            {
                auto first  = read_into(&f.reader, data, &same);
                auto second = read_into(&another, data, &other);
                CHECK_FALSE(first.resume());
                CHECK_FALSE(second.resume());
                f.arrive("go");
            });
        const auto res = co_await f.reader.read(data);
        REQUIRE_FALSE(res.err);
        CHECK(text(data, res) == "go");
        CHECK(same.err == std::errc::device_or_resource_busy);
        CHECK(other.err == std::errc::device_or_resource_busy);
    };
    sched.run_to_completion(test(&sched, listener.local_port()));
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
