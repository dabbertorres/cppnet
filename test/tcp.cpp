#include "tcp.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <netinet/in.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "dns/blocking_lookup.hpp"
#include "dns/resolver.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"
#include "listen.hpp"
#include "socket.hpp"
#include "util/defer.hpp"

namespace
{

using namespace std::chrono_literals;

using connect_result = std::expected<net::tcp_socket, std::error_code>;

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(2); }

// connect_error connects, and returns why it failed (or nothing, if it didn't).
net::coro::task<std::error_code>
connect_error(net::io::scheduler* sched, std::string host, std::string port, std::chrono::milliseconds timeout)
{
    auto conn = co_await net::tcp_socket::connect(sched, std::move(host), std::move(port), {.timeout = timeout});
    co_return conn.has_value() ? std::error_code{} : conn.error();
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

TEST_CASE("connecting to an unknown host fails to resolve", "[tcp][connect][.network]")
{
    net::io::scheduler sched{make_pool()};

    // ".invalid" names never resolve (RFC 6761).
    auto err = sched.run_to_completion(connect_error(&sched, "nonexistent.invalid", "80", 5s));
    CAPTURE(err.message());
    CHECK(err == net::dns::condition::not_found);
}

TEST_CASE("connecting takes only numeric ports", "[tcp][connect]")
{
    test::blocking_lookup lookup;
    lookup.release(); // answers at once

    net::io::scheduler sched{make_pool()};

    struct test_case
    {
        std::string host{};
        std::string port{};
    };

    // names as well as addresses: neither is looked up.
    auto tc = GENERATE(test_case{.host = "127.0.0.1", .port = "http"},
                       test_case{.host = "localhost.test", .port = "http"},
                       test_case{.host = "127.0.0.1", .port = ""},
                       test_case{.host = "127.0.0.1", .port = "80x"},
                       test_case{.host = "127.0.0.1", .port = "-1"},
                       test_case{.host = "127.0.0.1", .port = "65536"},
                       test_case{.host = "127.0.0.1", .port = " 80"});
    CAPTURE(tc.host, tc.port);

    const auto connect =
        [](net::io::scheduler* sched, test_case tc, net::dns::resolver* resolver) -> net::coro::task<std::error_code>
    {
        auto conn = co_await net::tcp_socket::connect(sched, tc.host, tc.port, {.resolver = resolver});
        co_return conn.has_value() ? std::error_code{} : conn.error();
    };

    CHECK(sched.run_to_completion(connect(&sched, tc, &lookup.resolver)) == std::errc::invalid_argument);
    CHECK(lookup.started.load() == 0);
}

TEST_CASE("connecting resolves names with the resolver given", "[tcp][connect]")
{
    // the name resolves to 127.0.0.1, where a listener is.
    test::blocking_lookup lookup;
    lookup.release();

    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());
    const auto port = std::to_string(listener->local_port());

    const auto connect = [](net::io::scheduler* sched,
                            std::string         port,
                            net::dns::resolver* resolver) -> net::coro::task<std::error_code>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "listener.test", std::move(port), {.resolver = resolver});
        co_return conn.has_value() ? std::error_code{} : conn.error();
    };

    auto err = sched.run_to_completion(connect(&sched, port, &lookup.resolver));
    CAPTURE(err.message());
    CHECK_FALSE(err);
    CHECK(lookup.finished.load() == 1);
}

TEST_CASE("connecting's timeout covers resolving", "[tcp][connect][timeout]")
{
    test::blocking_lookup lookup;

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto connect =
        [](net::io::scheduler*    sched,
           test::blocking_lookup* lookup) -> net::coro::task<std::pair<std::error_code, net::io::clock::duration>>
    {
        const auto start   = net::io::clock::now();
        auto       conn    = co_await net::tcp_socket::connect(sched,
                                                               "slow.test",
                                                               "80",
                                                               {.timeout = 200ms, .resolver = &lookup->resolver});
        const auto elapsed = net::io::clock::now() - start;

        lookup->release();
        co_return std::pair{conn.has_value() ? std::error_code{} : conn.error(), elapsed};
    };

    const auto [err, elapsed] = sched.run_to_completion(connect(&sched, &lookup));

    CAPTURE(err.message());
    CHECK(err == std::errc::timed_out);
    CHECK(elapsed >= 200ms);
    CHECK(elapsed < 5s);
    CHECK(lookup.started.load() == 1);
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

TEST_CASE("requesting stop ends resolving", "[tcp][connect][stop]")
{
    test::blocking_lookup lookup;

    std::stop_source   source;
    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto connect = [](net::io::scheduler*    sched,
                            test::blocking_lookup* lookup,
                            std::stop_token        stop) -> net::coro::task<std::error_code>
    {
        auto conn =
            co_await net::tcp_socket::connect(sched,
                                              "slow.test",
                                              "80",
                                              {.timeout = 10s, .stop = std::move(stop), .resolver = &lookup->resolver});

        lookup->release();
        co_return conn.has_value() ? std::error_code{} : conn.error();
    };

    // NOTE: declared after the scheduler, so it's joined first, once it has stopped the connect (which it does even if
    //       the lookup never starts: then its wait gives up).
    std::jthread stopper{[&]
                         {
                             (void)lookup.wait_started(1);
                             source.request_stop();
                         }};

    const auto start   = net::io::clock::now();
    const auto err     = sched.run_to_completion(connect(&sched, &lookup, source.get_token()));
    const auto elapsed = net::io::clock::now() - start;

    CAPTURE(err.message());
    CHECK(err == std::errc::operation_canceled);
    CHECK(elapsed < 5s);
    CHECK(lookup.started.load() == 1);
}

TEST_CASE("connecting with a stopped token fails without resolving", "[tcp][connect][stop]")
{
    std::stop_source source;
    source.request_stop();

    net::io::scheduler sched{make_pool()};

    // a name that never resolves: had it been looked up, it would have failed to resolve.
    const auto connect = [](net::io::scheduler* sched, std::stop_token stop) -> net::coro::task<std::error_code>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "nonexistent.invalid", "80", {.stop = std::move(stop)});
        co_return conn.has_value() ? std::error_code{} : conn.error();
    };

    CHECK(sched.run_to_completion(connect(&sched, source.get_token())) == std::errc::operation_canceled);
}

TEST_CASE("requesting stop ends connecting", "[tcp][connect][stop]")
{
    std::stop_source   source;
    net::io::scheduler sched{make_pool()};

    // a non-routable address: depending on the network, connecting either hangs (until stop is requested), or fails
    // straight away.
    const auto connect = [](net::io::scheduler* sched, std::stop_token stop) -> net::coro::task<std::error_code>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "10.255.255.1", "81", {.stop = std::move(stop)});
        co_return conn.has_value() ? std::error_code{} : conn.error();
    };

    std::thread stopper{[&]
                        {
                            std::this_thread::sleep_for(100ms);
                            source.request_stop();
                        }};

    auto start   = net::io::clock::now();
    auto err     = sched.run_to_completion(connect(&sched, source.get_token()));
    auto elapsed = net::io::clock::now() - start;
    stopper.join();

    CAPTURE(err.message());
    CHECK(err);
    CHECK(elapsed < 5s);
    if (err == std::errc::operation_canceled) CHECK(elapsed >= 100ms);
}

namespace
{

// connect_addresses_error connects to addresses, and returns why it failed (or nothing, if it didn't).
net::coro::task<std::error_code> connect_addresses_error(net::io::scheduler*              sched,
                                                         std::vector<net::ip_addr>        addresses,
                                                         std::uint16_t                    port,
                                                         net::tcp_socket::connect_options options)
{
    auto conn = co_await net::tcp_socket::connect(sched, addresses, port, options);
    co_return conn.has_value() ? std::error_code{} : conn.error();
}

// non-routable addresses: depending on the network, connecting to one either hangs, or fails straight away.
const net::ip_addr non_routable_1{
    net::ipv4_addr{10, 255, 255, 1}
};
const net::ip_addr non_routable_2{
    net::ipv4_addr{10, 255, 255, 2}
};
const net::ip_addr non_routable_3{
    net::ipv4_addr{10, 255, 255, 3}
};

// block holds one of the scheduler's blocking threads until release is set, counting it in blocked once it has.
net::coro::task<void> block(net::io::scheduler* sched, std::atomic<bool>* release, std::atomic<int>* blocked)
{
    co_await sched->run_blocking(
        [=]
        {
            blocked->fetch_add(1);
            blocked->notify_all();
            release->wait(false);
        });
}

}

TEST_CASE("each attempt to connect gets a share of the time left", "[tcp][connect][timeout]")
{
    struct test_case
    {
        std::chrono::milliseconds left      = {};
        std::size_t               remaining = 0;
        std::chrono::milliseconds expected  = {};
    };

    auto tc = GENERATE(test_case{.left = 10s, .remaining = 1, .expected = 10s},
                       test_case{.left = 10s, .remaining = 2, .expected = 5s},
                       // a share under 2 seconds is raised to 2...
                       test_case{.left = 10s, .remaining = 10, .expected = 2s},
                       test_case{.left = 3s, .remaining = 2, .expected = 2s},
                       // ... but never past the end.
                       test_case{.left = 1s, .remaining = 3, .expected = 1s},
                       test_case{.left = 2s, .remaining = 2, .expected = 2s});

    CAPTURE(tc.left.count(), tc.remaining);

    const auto now = net::io::clock::now();
    CHECK(net::detail::attempt_deadline(now, now + tc.left, tc.remaining) - now == tc.expected);
}

TEST_CASE("connecting to addresses tries the next after a refusal", "[tcp][connect]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // nothing listens on ::1 (and it may not even be available): the attempt fails, and the next one is made.
    const std::vector<net::ip_addr> addresses{net::ipv6_addr::loopback(), net::ipv4_addr::loopback()};

    auto err = sched.run_to_completion(connect_addresses_error(&sched, addresses, listener->local_port(), {}));
    CAPTURE(err.message());
    CHECK_FALSE(err);
}

TEST_CASE("connecting to addresses leaves time for the next if one doesn't answer", "[tcp][connect][timeout]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // the first attempt gets 2 of the 4 seconds, rather than all of them, as it would if they weren't shared.
    const std::vector<net::ip_addr> addresses{non_routable_1, net::ipv4_addr::loopback()};

    auto start = net::io::clock::now();
    auto err =
        sched.run_to_completion(connect_addresses_error(&sched, addresses, listener->local_port(), {.timeout = 4s}));
    auto elapsed = net::io::clock::now() - start;

    CAPTURE(err.message());
    CHECK_FALSE(err);
    CHECK(elapsed < 4s);
}

TEST_CASE("connecting to addresses gives up at its timeout", "[tcp][connect][timeout]")
{
    net::io::scheduler sched{make_pool()};

    const std::vector<net::ip_addr> addresses{non_routable_1, non_routable_2};

    auto start   = net::io::clock::now();
    auto err     = sched.run_to_completion(connect_addresses_error(&sched, addresses, 81, {.timeout = 200ms}));
    auto elapsed = net::io::clock::now() - start;

    CAPTURE(err.message());
    CHECK(err);
    CHECK(elapsed < 2s);
    if (err == std::errc::timed_out) CHECK(elapsed >= 200ms);
}

TEST_CASE("requesting stop ends connecting to addresses", "[tcp][connect][stop]")
{
    std::stop_source   source;
    net::io::scheduler sched{make_pool()};

    const std::vector<net::ip_addr> addresses{non_routable_1, non_routable_2, non_routable_3};

    std::jthread stopper{[&]
                         {
                             std::this_thread::sleep_for(100ms);
                             source.request_stop();
                         }};

    // each attempt's share is over 3 seconds: stopping ends the first, and the rest aren't tried.
    auto start = net::io::clock::now();
    auto err   = sched.run_to_completion(
        connect_addresses_error(&sched, addresses, 81, {.timeout = 10s, .stop = source.get_token()}));
    auto elapsed = net::io::clock::now() - start;

    CAPTURE(err.message());
    CHECK(err);
    CHECK(elapsed < 3s);
    if (err == std::errc::operation_canceled) CHECK(elapsed >= 100ms);
}

TEST_CASE("connecting to addresses with a stopped token fails without connecting", "[tcp][connect][stop]")
{
    std::stop_source source;
    source.request_stop();

    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    const std::vector<net::ip_addr> addresses{net::ipv4_addr::loopback()};

    auto err = sched.run_to_completion(
        connect_addresses_error(&sched, addresses, listener->local_port(), {.stop = source.get_token()}));
    CHECK(err == std::errc::operation_canceled);
}

TEST_CASE("connecting to addresses only tries those of the protocol asked for", "[tcp][connect]")
{
    net::io::scheduler sched{make_pool()};

    SECTION("none of its family")
    {
        const std::vector<net::ip_addr> addresses{net::ipv4_addr::loopback()};

        auto err =
            sched.run_to_completion(connect_addresses_error(&sched, addresses, 81, {.proto = net::protocol::ipv6}));
        CHECK(err == std::errc::address_family_not_supported);
    }

    SECTION("none at all")
    {
        auto err = sched.run_to_completion(connect_addresses_error(&sched, {}, 81, {}));
        CHECK(err == std::errc::invalid_argument);
    }

    SECTION("an IP address as the host")
    {
        const auto connect = [](net::io::scheduler* sched) -> net::coro::task<std::error_code>
        {
            auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", "81", {.proto = net::protocol::ipv6});
            co_return conn.has_value() ? std::error_code{} : conn.error();
        };

        CHECK(sched.run_to_completion(connect(&sched)) == std::errc::address_family_not_supported);
    }
}

TEST_CASE("connecting to an IP address doesn't use a blocking thread", "[tcp][connect]")
{
    std::atomic<bool> release{false};
    std::atomic<int>  blocked{0};

    net::io::scheduler sched{make_pool()};

    // releases the blocking threads, after the test or 10 seconds, whichever is first, so a connect that waits for one
    // fails the test rather than hanging it. (Destroyed before the scheduler, which waits for the tasks they block.)
    std::jthread releaser{[&](const std::stop_token& stop)
                          {
                              std::mutex                  mu;
                              std::condition_variable_any cv;
                              std::unique_lock            lock{mu};
                              cv.wait_for(lock, stop, 10s, [] { return false; });

                              release.store(true);
                              release.notify_all();
                          }};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());
    const auto port = std::to_string(listener->local_port());

    // the scheduler sets aside 4 threads for blocking calls: hold all of them.
    constexpr int blocking_threads = 4;
    for (int i = 0; i < blocking_threads; ++i) sched.spawn(block(&sched, &release, &blocked));
    for (int seen = blocked.load(); seen < blocking_threads; seen = blocked.load()) blocked.wait(seen);

    // connects, then releases the blocking threads (which run_to_completion() waits for), saying whether they already
    // were: if connecting waited for one, they were, by the releaser.
    const auto connect = [](net::io::scheduler* sched,
                            std::string         port,
                            std::atomic<bool>*  release) -> net::coro::task<std::pair<std::error_code, bool>>
    {
        auto err      = co_await connect_error(sched, "127.0.0.1", std::move(port), 5s);
        auto released = release->exchange(true);
        release->notify_all();
        co_return std::pair{err, released};
    };

    auto [err, released_first] = sched.run_to_completion(connect(&sched, port, &release));

    CAPTURE(err.message());
    CHECK_FALSE(err);
    CHECK_FALSE(released_first);
}
