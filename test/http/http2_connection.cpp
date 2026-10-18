#include "http/http2_connection.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/connection.hpp"
#include "http/http2.hpp"
#include "http/http2_session.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace http2  = net::http::http2;
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
