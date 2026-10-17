#include "listen.hpp"

#include <expected>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "exception.hpp"
#include "io/scheduler.hpp"
#include "tcp.hpp"

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(2); }

}

TEST_CASE("listener::make listens, on a port the OS chooses", "[listener]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());
    CHECK(listener->listening());
    REQUIRE(listener->local_port() != 0);

    // connects, and is accepted.
    const auto connect_and_accept = [](net::io::scheduler* sched,
                                       net::listener*      listener) -> net::coro::task<std::error_code>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(listener->local_port()));
        if (!conn.has_value()) co_return conn.error();

        auto accepted = co_await listener->accept();
        co_return accepted.has_value() ? std::error_code{} : accepted.error();
    };

    auto err = sched.run_to_completion(connect_and_accept(&sched, &*listener));
    CAPTURE(err.message());
    CHECK_FALSE(err);
}

TEST_CASE("listener::make reports why it can't listen", "[listener]")
{
    net::io::scheduler sched{make_pool()};

    auto first = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(first.has_value());

    auto taken = net::listener::make(&sched, "127.0.0.1", std::to_string(first->local_port()));
    REQUIRE_FALSE(taken.has_value());
    CHECK(taken.error() == std::errc::address_in_use);

    // .invalid never resolves (RFC 6761).
    auto unknown = net::listener::make(&sched, "nothing.invalid", "0");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().category() == net::gai_category());

    // the constructors throw instead.
    CHECK_THROWS_AS(net::listener(&sched, "127.0.0.1", std::to_string(first->local_port()), net::network::tcp),
                    std::system_error);
}
