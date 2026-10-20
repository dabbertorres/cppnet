#include "tcp.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <stop_token>
#include <string>
#include <string_view>
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
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"
#include "listen.hpp"
#include "sockaddr.hpp"
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

    // the first attempt gets 2 of the 4 seconds, rather than all of them, as it would if they weren't shared. (Without
    // racing them, which would start the second after the attempt delay.)
    const std::vector<net::ip_addr> addresses{non_routable_1, net::ipv4_addr::loopback()};

    auto start = net::io::clock::now();
    auto err   = sched.run_to_completion(
        connect_addresses_error(&sched, addresses, listener->local_port(), {.timeout = 4s, .attempt_delay = 0ms}));
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

namespace
{

// fake_network stands in for connect() (connect_options::system_connect), so attempts to connect fail, hang or connect
// as a test needs, whatever the network: each address it's given (e.g. one of the documentation's, 192.0.2.0/24 and
// 2001:db8::/32, never actually connected to) does what its behaviour says, and the attempts are recorded.
class fake_network
{
public:
    enum class behaviour : std::uint8_t
    {
        // fails at once, with std::errc::connection_refused.
        refuse,
        // connecting never ends (connect() isn't called, so the socket never connects), unless it's connected for
        // real (see connect_hanging()).
        hang,
        // connects to 127.0.0.1 on the listening port (for an IPv4 address): connected, or connecting.
        answer,
        // connects to 127.0.0.1 on a port nothing listens on (for an IPv4 address): refused, usually once connecting
        // has started.
        refuse_later,
        // fails at once, with std::errc::network_unreachable.
        unreachable,
    };

    struct attempt
    {
        net::ip_addr               addr;
        net::io::clock::time_point when;
    };

    explicit fake_network(std::vector<std::pair<net::ip_addr, behaviour>> addresses,
                          std::uint16_t                                   listening = 0,
                          std::uint16_t                                   closed    = 0)
        : addresses{std::move(addresses)}
        , listening{listening}
        , closed{closed}
    {}

    // hook is what connect_options::system_connect is set to. It refers to the network, which must outlive the
    // connects given it.
    net::detail::connect_function hook()
    {
        return [this](net::io::handle fd, const sockaddr* addr, socklen_t size) { return connect(fd, addr, size); };
    }

    // addresses_given are the network's addresses, in its order.
    [[nodiscard]] std::vector<net::ip_addr> addresses_given() const
    {
        std::vector<net::ip_addr> given;
        given.reserve(addresses.size());
        for (const auto& [addr, _] : addresses) given.push_back(addr);
        return given;
    }

    [[nodiscard]] std::vector<attempt> attempts() const
    {
        const std::scoped_lock lock{mu};
        return made;
    }

    [[nodiscard]] std::vector<net::ip_addr> tried() const
    {
        const auto                all = attempts();
        std::vector<net::ip_addr> addrs;
        addrs.reserve(all.size());
        for (const auto& a : all) addrs.push_back(a.addr);
        return addrs;
    }

    // hanging are the sockets of the attempts that hang, in the order they were made.
    [[nodiscard]] std::vector<net::io::handle> hanging() const
    {
        const std::scoped_lock lock{mu};
        return hung;
    }

    // on_attempt is called (on the connecting thread, before the attempt's behaviour) with the number of attempts made
    // so far, this one included. (Catch2's assertions aren't for other threads: it records what to check after.)
    std::function<void(std::size_t)> on_attempt;

    // connect_hanging connects the first attempt that hung for real, to 127.0.0.1 on port, when the count'th attempt
    // starts (see on_attempt), recording connect()'s result in result.
    void connect_hanging(std::size_t count, std::uint16_t port, std::error_code* result)
    {
        on_attempt = [this, count, port, result](std::size_t made)
        {
            if (made != count) return;

            const auto fds = hanging();
            *result        = fds.empty() ? std::make_error_code(std::errc::bad_file_descriptor)
                                         : connect_loopback(fds.front(), port);
        };
    }

    // connect_loopback connects fd for real, to 127.0.0.1 on port.
    static std::error_code connect_loopback(net::io::handle fd, std::uint16_t port)
    {
        sockaddr_in sin{};
        sin.sin_family      = AF_INET;
        sin.sin_port        = htons(port);
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::connect(fd, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin)) == 0) return {};
        return {errno, std::system_category()};
    }

private:
    std::error_code connect(net::io::handle fd, const sockaddr* addr, socklen_t size)
    {
        auto to = net::detail::from_sockaddr(addr, size);
        if (!to.has_value()) return to.error();

        auto what = behaviour::refuse;
        for (const auto& [a, b] : addresses)
        {
            if (a == to->addr) what = b;
        }

        std::size_t count = 0;
        {
            const std::scoped_lock lock{mu};
            made.push_back({.addr = to->addr, .when = net::io::clock::now()});
            if (what == behaviour::hang) hung.push_back(fd);
            count = made.size();
        }

        if (on_attempt) on_attempt(count);

        switch (what)
        {
        case behaviour::refuse: return std::make_error_code(std::errc::connection_refused);
        case behaviour::hang: return std::make_error_code(std::errc::operation_in_progress);
        case behaviour::answer: return connect_loopback(fd, listening);
        case behaviour::refuse_later: return connect_loopback(fd, closed);
        case behaviour::unreachable: return std::make_error_code(std::errc::network_unreachable);
        }

        return std::make_error_code(std::errc::invalid_argument);
    }

    std::vector<std::pair<net::ip_addr, behaviour>> addresses;
    std::uint16_t                                   listening;
    std::uint16_t                                   closed;

    mutable std::mutex           mu;
    std::vector<attempt>         made;
    std::vector<net::io::handle> hung;
};

net::ip_addr addr(std::string_view text)
{
    auto parsed = net::ip_addr::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

// connect_through connects to the network's addresses through it (on port 80, which it ignores), with options, and
// returns the connection or why it failed, and how long it took.
net::coro::task<std::pair<connect_result, net::io::clock::duration>>
connect_through(net::io::scheduler* sched, fake_network* network, net::tcp_socket::connect_options options)
{
    options.system_connect = network->hook();

    const auto start = net::io::clock::now();
    auto       conn  = co_await net::tcp_socket::connect(sched, network->addresses_given(), 80, options);
    co_return std::pair{std::move(conn), net::io::clock::now() - start};
}

using enum fake_network::behaviour;

}

TEST_CASE("racing attempts interleaves the addresses by family", "[tcp][connect][happy-eyeballs]")
{
    struct test_case
    {
        std::vector<std::string_view> given;
        std::vector<std::string_view> expected;
    };

    auto tc = GENERATE(
        // the first address's family first...
        test_case{
            .given    = {"2001:db8::1", "2001:db8::2", "2001:db8::3", "192.0.2.1",   "192.0.2.2"},
            .expected = {"2001:db8::1",   "192.0.2.1", "2001:db8::2", "192.0.2.2", "2001:db8::3"},
    },
        test_case{
            .given    = {"192.0.2.1", "192.0.2.2", "2001:db8::1", "2001:db8::2"},
            .expected = {"192.0.2.1", "2001:db8::1", "192.0.2.2", "2001:db8::2"},
        },
        // ... each family's in their order, alternating while both have some left.
        test_case{
            .given    = {"2001:db8::1", "192.0.2.1", "192.0.2.2", "192.0.2.3", "2001:db8::2"},
            .expected = {"2001:db8::1", "192.0.2.1", "2001:db8::2", "192.0.2.2", "192.0.2.3"},
        },
        test_case{
            .given    = {"192.0.2.1", "192.0.2.2", "192.0.2.3"},
            .expected = {"192.0.2.1", "192.0.2.2", "192.0.2.3"},
        });

    std::vector<std::pair<net::ip_addr, fake_network::behaviour>> addresses;
    addresses.reserve(tc.given.size());
    for (auto text : tc.given) addresses.emplace_back(addr(text), refuse);

    std::vector<net::ip_addr> expected;
    expected.reserve(tc.expected.size());
    for (auto text : tc.expected) expected.push_back(addr(text));

    fake_network       network{addresses};
    net::io::scheduler sched{make_pool()};

    // each fails at once, so the next starts at once: the attempt delay is never waited for.
    auto [conn, elapsed] = sched.run_to_completion(connect_through(&sched, &network, {.attempt_delay = 10s}));

    REQUIRE_FALSE(conn.has_value());
    CAPTURE(conn.error().message());
    CHECK(conn.error() == std::errc::connection_refused);
    CHECK(network.tried() == expected);
    CHECK(elapsed < 5s);
}

TEST_CASE("an attempt delay of 0 tries the addresses in their order, one at a time", "[tcp][connect][happy-eyeballs]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    SECTION("in their order")
    {
        fake_network network{
            {{addr("192.0.2.1"), refuse}, {addr("192.0.2.2"), refuse}, {addr("2001:db8::1"), refuse}}
        };

        auto [conn, _] = sched.run_to_completion(connect_through(&sched, &network, {.attempt_delay = 0ms}));

        REQUIRE_FALSE(conn.has_value());
        CHECK(conn.error() == std::errc::connection_refused);
        CHECK(network.tried() == network.addresses_given());
    }

    SECTION("one at a time")
    {
        // the first attempt's share is all of the second (at least 2 seconds, but no more than is left), so the
        // second is never tried.
        fake_network network{
            {{addr("192.0.2.1"), hang}, {addr("192.0.2.2"), answer}},
            listener->local_port()
        };

        auto [conn, elapsed] =
            sched.run_to_completion(connect_through(&sched, &network, {.timeout = 1s, .attempt_delay = 0ms}));

        REQUIRE_FALSE(conn.has_value());
        CAPTURE(conn.error().message());
        CHECK(conn.error() == std::errc::timed_out);
        CHECK(elapsed >= 1s);
        CHECK(network.tried() == std::vector{addr("192.0.2.1")});
    }
}

TEST_CASE("racing attempts starts the next after the attempt delay", "[tcp][connect][happy-eyeballs]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // the first hangs: the second starts once it has had the delay, and connects, though the first goes on.
    fake_network network{
        {{addr("2001:db8::1"), hang}, {addr("192.0.2.1"), answer}},
        listener->local_port()
    };

    constexpr auto delay = 100ms;

    auto [conn, elapsed] = sched.run_to_completion(connect_through(&sched, &network, {.attempt_delay = delay}));

    REQUIRE(conn.has_value());
    CHECK(elapsed < 5s);

    const auto attempts = network.attempts();
    REQUIRE(attempts.size() == 2);
    CHECK(attempts[0].addr == addr("2001:db8::1"));
    CHECK(attempts[1].addr == addr("192.0.2.1"));

    // NOTE: the delay is counted from just before the first attempt, so a little of it may have passed by then.
    CHECK(attempts[1].when - attempts[0].when >= delay - 10ms);
}

TEST_CASE("racing attempts starts the next at once when the latest fails", "[tcp][connect][happy-eyeballs]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    const auto closed = static_cast<std::uint16_t>(std::stoi(closed_port()));

    constexpr auto delay = 500ms;

    SECTION("while it's the only one")
    {
        // the first is refused (once it started, or at once): the second starts then, not after the delay.
        fake_network network{
            {{addr("192.0.2.1"), refuse_later}, {addr("192.0.2.2"), answer}},
            listener->local_port(),
            closed
        };

        auto [conn, elapsed] = sched.run_to_completion(connect_through(&sched, &network, {.attempt_delay = delay}));

        REQUIRE(conn.has_value());
        CHECK(elapsed < delay);
        CHECK(network.tried().size() == 2);
    }

    SECTION("while others go on")
    {
        // the first hangs, so the second starts after the delay; it's refused, so the third starts then, not after
        // another delay.
        fake_network network{
            {{addr("192.0.2.1"), hang}, {addr("192.0.2.2"), refuse_later}, {addr("192.0.2.3"), answer}},
            listener->local_port(),
            closed
        };

        auto [conn, elapsed] = sched.run_to_completion(connect_through(&sched, &network, {.attempt_delay = delay}));

        REQUIRE(conn.has_value());
        CHECK(elapsed < 2 * delay);

        const auto attempts = network.attempts();
        REQUIRE(attempts.size() == 3);
        CHECK(attempts[1].when - attempts[0].when >= delay - 10ms);
        CHECK(attempts[2].when - attempts[1].when < delay / 2);
    }

    SECTION("but not when an earlier one fails")
    {
        // the second hangs, so the first failing doesn't start the third: it starts after the delay.
        fake_network network{
            {{addr("192.0.2.1"), hang}, {addr("192.0.2.2"), hang}, {addr("192.0.2.3"), answer}},
            listener->local_port(),
            closed
        };

        // the first attempt is refused as the second starts: connected for real, to the closed port.
        std::error_code refusing;
        network.connect_hanging(2, closed, &refusing);

        auto [conn, elapsed] = sched.run_to_completion(connect_through(&sched, &network, {.attempt_delay = delay}));

        CAPTURE(refusing.message());
        CHECK(refusing == std::errc::operation_in_progress);
        REQUIRE(conn.has_value());

        const auto attempts = network.attempts();
        REQUIRE(attempts.size() == 3);
        CHECK(attempts[2].when - attempts[1].when >= delay - 10ms);
    }
}

TEST_CASE("racing attempts takes the first to connect, and closes the rest", "[tcp][connect][happy-eyeballs]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    const auto port = listener->local_port();

    // the first hangs until the second starts, then both connect (for real, to the listener) at once: whichever is
    // first wins, and the other is closed (stopped, or closed once it connected).
    fake_network network{
        {{addr("192.0.2.1"), hang}, {addr("192.0.2.2"), answer}},
        port
    };

    std::error_code connecting;
    network.connect_hanging(2, port, &connecting);

    struct outcome
    {
        std::error_code              connected;
        std::error_code              wrote;
        std::vector<net::io::result> read;
    };

    // connects, writes a byte to the connection it got, then reads from each connection the listener accepted: the
    // winner's gets the byte, the loser's ends.
    const auto run =
        [](net::io::scheduler* sched, fake_network* network, const net::listener* listener) -> net::coro::task<outcome>
    {
        outcome result;

        auto [conn, _] = co_await connect_through(sched, network, {.attempt_delay = 50ms});
        if (!conn.has_value())
        {
            result.connected = conn.error();
            co_return result;
        }

        const std::array<std::byte, 1> hello{std::byte{'x'}};
        auto                           wrote = co_await conn->write(hello);
        result.wrote                         = wrote.err;

        for (int i = 0; i < 2; ++i)
        {
            auto accepted = co_await listener->accept();
            if (!accepted.has_value())
            {
                result.read.push_back({.err = accepted.error()});
                continue;
            }
            accepted->set_read_timeout(5s);

            std::array<std::byte, 1> buf{};
            result.read.push_back(co_await accepted->read(buf));
        }

        co_return result;
    };

    const auto result = sched.run_to_completion(run(&sched, &network, &*listener));

    CAPTURE(connecting.message(), result.connected.message(), result.wrote.message());
    CHECK((!connecting || connecting == std::errc::operation_in_progress));
    REQUIRE_FALSE(result.connected);
    REQUIRE_FALSE(result.wrote);
    REQUIRE(result.read.size() == 2);

    std::size_t won  = 0;
    std::size_t lost = 0;
    for (const auto& read : result.read)
    {
        CAPTURE(read.count, read.err.message());
        if (read.count == 1 && !read.err) ++won;
        else if (read.count == 0 && read.err == net::io::status_condition::closed) ++lost;
    }
    CHECK(won == 1);
    CHECK(lost == 1);
}

TEST_CASE("racing attempts fails with the last attempt's error", "[tcp][connect][happy-eyeballs]")
{
    net::io::scheduler sched{make_pool()};

    const auto closed = static_cast<std::uint16_t>(std::stoi(closed_port()));

    SECTION("one after another")
    {
        // the first fails once it's started (usually), the second at once.
        fake_network network{
            {{addr("192.0.2.1"), refuse_later}, {addr("2001:db8::1"), unreachable}},
            0,
            closed
        };

        auto [conn, elapsed] = sched.run_to_completion(connect_through(&sched, &network, {.attempt_delay = 5s}));

        REQUIRE_FALSE(conn.has_value());
        CAPTURE(conn.error().message());
        CHECK(conn.error() == std::errc::network_unreachable);
        CHECK(elapsed < 5s);
        CHECK(network.tried().size() == 2);
    }

    SECTION("while racing")
    {
        // the first hangs until the second starts, then both are refused (connected for real to the closed port), each
        // once it's started, going on together.
        fake_network network{
            {{addr("192.0.2.1"), hang}, {addr("192.0.2.2"), refuse_later}},
            0,
            closed
        };

        std::error_code refusing;
        network.connect_hanging(2, closed, &refusing);

        auto [conn, elapsed] =
            sched.run_to_completion(connect_through(&sched, &network, {.timeout = 5s, .attempt_delay = 50ms}));

        CAPTURE(refusing.message());
        CHECK(refusing == std::errc::operation_in_progress);
        REQUIRE_FALSE(conn.has_value());
        CAPTURE(conn.error().message());
        CHECK(conn.error() == std::errc::connection_refused);
        CHECK(elapsed < 5s);
        CHECK(network.tried().size() == 2);
    }
}

TEST_CASE("racing attempts ends at the timeout", "[tcp][connect][happy-eyeballs][timeout]")
{
    net::io::scheduler sched{make_pool()};

    SECTION("with several going")
    {
        fake_network network{
            {{addr("2001:db8::1"), hang}, {addr("192.0.2.1"), hang}}
        };

        auto [conn, elapsed] =
            sched.run_to_completion(connect_through(&sched, &network, {.timeout = 300ms, .attempt_delay = 50ms}));

        REQUIRE_FALSE(conn.has_value());
        CAPTURE(conn.error().message());
        CHECK(conn.error() == std::errc::timed_out);
        CHECK(elapsed >= 300ms);
        CHECK(elapsed < 3s);
        CHECK(network.tried().size() == 2);
    }

    SECTION("with one address")
    {
        // the one attempt has all of the time.
        fake_network network{{{addr("192.0.2.1"), hang}}};

        auto [conn, elapsed] =
            sched.run_to_completion(connect_through(&sched, &network, {.timeout = 300ms, .attempt_delay = 50ms}));

        REQUIRE_FALSE(conn.has_value());
        CAPTURE(conn.error().message());
        CHECK(conn.error() == std::errc::timed_out);
        CHECK(elapsed >= 300ms);
        CHECK(elapsed < 3s);
        CHECK(network.tried().size() == 1);
    }
}

TEST_CASE("requesting stop ends racing attempts", "[tcp][connect][happy-eyeballs][stop]")
{
    std::stop_source   source;
    net::io::scheduler sched{make_pool()};

    // none of the attempts ends by itself: stop is requested once all three have started (at 0, 50 and 100 ms), and
    // connecting waits for them.
    fake_network network{
        {{addr("2001:db8::1"), hang}, {addr("192.0.2.1"), hang}, {addr("2001:db8::2"), hang}}
    };

    std::atomic<std::size_t> started{0};
    network.on_attempt = [&](std::size_t count)
    {
        started.store(count);
        started.notify_all();
    };

    std::jthread stopper{[&]
                         {
                             for (auto seen = started.load(); seen < 3; seen = started.load()) started.wait(seen);
                             std::this_thread::sleep_for(50ms);
                             source.request_stop();
                         }};

    auto [conn, elapsed] = sched.run_to_completion(
        connect_through(&sched, &network, {.timeout = 10s, .attempt_delay = 50ms, .stop = source.get_token()}));

    // lets the stopper go, should connecting have ended before all three started.
    started.store(3);
    started.notify_all();

    REQUIRE_FALSE(conn.has_value());
    CAPTURE(conn.error().message());
    CHECK(conn.error() == std::errc::operation_canceled);
    CHECK(elapsed < 5s);
    CHECK(network.tried().size() == 3);
}
