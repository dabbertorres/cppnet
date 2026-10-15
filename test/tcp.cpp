#include "tcp.hpp"

#include <array>
#include <chrono>
#include <compare>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <netinet/in.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "exception.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "socket.hpp"

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

TEST_CASE("socket addresses are only the address", "[tcp][addr]")
{
    net::io::scheduler sched{make_pool()};

    // "::1" may not be available (e.g. in a container without IPv6).
    auto host = GENERATE(as<std::string>{}, "127.0.0.1", "::1");
    CAPTURE(host);

    std::optional<net::listener> listener;
    try
    {
        listener.emplace(&sched, host, "0", net::network::tcp);
        listener->listen(1);
    }
    catch (const std::exception& e)
    {
        SKIP("can't listen on " + host + ": " + e.what());
    }

    sockaddr_storage addr{};
    socklen_t        size = sizeof(addr);
    REQUIRE(::getsockname(listener->native_handle(), reinterpret_cast<sockaddr*>(&addr), &size) == 0);
    auto port = std::to_string(ntohs(addr.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port
                                                                : reinterpret_cast<sockaddr_in*>(&addr)->sin_port));

    auto connect_and_accept = [](net::io::scheduler* sched, net::listener* listener, std::string host, std::string port)
        -> net::coro::task<std::pair<std::string, std::string>>
    {
        auto conn = co_await net::tcp_socket::connect(sched, host, port);
        if (!conn.has_value()) co_return std::pair{"connect failed: " + conn.error().message(), std::string{}};

        auto accepted = co_await listener->accept();
        if (!accepted.has_value()) co_return std::pair{"accept failed: " + accepted.error().message(), std::string{}};

        co_return std::pair{conn->local_addr(), accepted->remote_addr()};
    };

    auto [local, remote] = sched.run_to_completion(connect_and_accept(&sched, &*listener, host, port));

    // in particular, no padding up to the longest address.
    CHECK(local == host);
    CHECK(remote == host);
}

TEST_CASE("an unbound Unix socket's address is empty", "[tcp][addr]")
{
    net::io::scheduler sched{make_pool()};

    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);

    net::socket a{&sched, fds[0]};
    net::socket b{&sched, fds[1]};

    CHECK(a.local_addr().empty());
    CHECK(b.remote_addr().empty());
}
