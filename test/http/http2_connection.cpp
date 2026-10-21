#include "http/http2_connection.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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
    using http2_connection_base::on_data;
    using http2_connection_base::remove_stream;

protected:
    void on_headers(http2::stream_id /*id*/,
                    http2::block_kind /*kind*/,
                    std::span<const net::http::hpack::field> /*fields*/,
                    bool /*end_stream*/) override
    {}
    void on_goaway(http2::stream_id /*last*/, std::error_code /*err*/, std::string_view /*debug*/) override {}
    void on_ping_ack(std::span<const std::byte, 8> /*opaque*/) override {}
    void on_priority_update(http2::stream_id /*id*/, net::http::priority /*p*/) override {}

    std::optional<net::io::clock::time_point> check_connection(net::io::clock::time_point /*now*/) override
    {
        return std::nullopt;
    }
};

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
