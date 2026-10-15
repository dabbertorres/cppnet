#include "tcp.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <system_error>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "exception.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"

namespace
{

using namespace std::chrono_literals;

using connect_result = std::expected<net::tcp_socket, std::error_condition>;

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(2); }

// connect_error connects, and returns why it failed (or nothing, if it didn't).
net::coro::task<std::error_condition>
connect_error(net::io::scheduler* sched, std::string host, std::string port, std::chrono::milliseconds timeout)
{
    auto conn = co_await net::tcp_socket::connect(sched, std::move(host), std::move(port), {.timeout = timeout});
    co_return conn.has_value() ? std::error_condition{} : conn.error();
}

std::string closed_port()
{
    // bind a port, and then let it go: nothing is listening on it afterwards.
    net::io::scheduler sched{make_pool()};
    net::listener      listener{&sched, "127.0.0.1", "0", net::network::tcp};

    sockaddr_in addr{};
    socklen_t   size = sizeof(addr);
    REQUIRE(::getsockname(listener.native_handle(), reinterpret_cast<sockaddr*>(&addr), &size) == 0);
    return std::to_string(ntohs(addr.sin_port));
}

}

TEST_CASE("connecting to a port nothing listens on is refused", "[tcp][connect]")
{
    auto port = closed_port();

    net::io::scheduler sched{make_pool()};
    CHECK(sched.run_to_completion(connect_error(&sched, "127.0.0.1", port, 5s)) == std::errc::connection_refused);
}

TEST_CASE("connecting to an unknown host fails to resolve", "[tcp][connect]")
{
    net::io::scheduler sched{make_pool()};

    // ".invalid" names never resolve (RFC 6761).
    auto err = sched.run_to_completion(connect_error(&sched, "nonexistent.invalid", "80", 5s));
    CHECK(err.category() == net::gai_category());
}

TEST_CASE("connecting gives up at its timeout", "[tcp][connect]")
{
    net::io::scheduler sched{make_pool()};

    // a non-routable address: depending on the network, connecting either hangs, or fails straight away.
    auto start   = net::io::clock::now();
    auto err     = sched.run_to_completion(connect_error(&sched, "10.255.255.1", "81", 200ms));
    auto elapsed = net::io::clock::now() - start;

    CAPTURE(err.message());
    CHECK(err);
    CHECK(elapsed < 2s);
    if (err == std::errc::timed_out) CHECK(elapsed >= 200ms);
}
