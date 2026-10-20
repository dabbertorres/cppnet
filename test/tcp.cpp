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

#include <netdb.h>
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
#include "exception.hpp"
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
    CHECK(lookup.finished.load() >= 1); // (a call for each family, unless it connected before the second was made)
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
    CHECK(lookup.started.load() == 2); // a call for each family
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
                             (void)lookup.wait_started(2); // a call for each family
                             source.request_stop();
                         }};

    const auto start   = net::io::clock::now();
    const auto err     = sched.run_to_completion(connect(&sched, &lookup, source.get_token()));
    const auto elapsed = net::io::clock::now() - start;

    CAPTURE(err.message());
    CHECK(err == std::errc::operation_canceled);
    CHECK(elapsed < 5s);
    CHECK(lookup.started.load() == 2);
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

namespace
{

// connecting is a connect to a name (see connect_named()), as a task, and how it ended once it has: why it failed
// (none, if it connected: the connection is then closed, as the scheduler's sockets mustn't outlive it), and how long
// it took.
struct connecting
{
    std::error_code            err{};
    net::io::clock::duration   elapsed{};
    std::atomic<std::uint64_t> done{0};
};

// connect_named connects to "host.test", which lookup resolves (to what a test makes its answer: e.g. the network's
// addresses), through network (on port 80, which it ignores), with options, recording what it returned in out.
net::coro::task<> connect_named(net::io::scheduler*              sched,
                                fake_network*                    network,
                                test::blocking_lookup*           lookup,
                                net::tcp_socket::connect_options options,
                                connecting*                      out)
{
    options.system_connect = network->hook();
    options.resolver       = &lookup->resolver;

    const auto start = net::io::clock::now();
    auto       conn  = co_await net::tcp_socket::connect(sched, "host.test", "80", options);
    out->elapsed     = net::io::clock::now() - start;
    out->err         = conn.has_value() ? std::error_code{} : conn.error();

    out->done.store(1);
    sched->notify_all(out->done);
}

// wait_done waits (in a task) until c's connect has returned, returning whether it has: not if the scheduler's
// stopped, or it hasn't within 10 seconds.
net::coro::task<bool> wait_done(net::io::scheduler* sched, const connecting* c)
{
    const auto until = net::io::clock::now() + 10s;
    while (c->done.load() == 0)
    {
        if (auto err = co_await sched->wait_on(c->done, 0, until); err) co_return false;
    }
    co_return true;
}

// wait_calls waits (in a task) until lookup has started n calls, returning whether it has: not if the scheduler's
// stopped, or it hasn't within 10 seconds.
net::coro::task<bool> wait_calls(net::io::scheduler* sched, const test::blocking_lookup* lookup, int n)
{
    const auto until = net::io::clock::now() + 10s;
    while (lookup->started.load() < n)
    {
        if (net::io::clock::now() >= until) co_return false;
        if (auto err = co_await sched->sleep_until(net::io::clock::now() + 1ms); err) co_return false;
    }
    co_return true;
}

// wait_found waits (in a task) until res has found something new after version, returning what it has then: what it
// had, if the scheduler's stopped, or nothing new came within 10 seconds.
net::coro::task<net::dns::detail::found>
wait_found(net::io::scheduler* sched, const net::dns::detail::resolution* res, std::uint64_t version)
{
    const auto until = net::io::clock::now() + 10s;
    while (res->news().load() == version)
    {
        if (auto err = co_await sched->wait_on(res->news(), version, until); err) break;
    }
    co_return res->look();
}

// joined starts c's connect, then joins the lookup it starts, so a test sees what it finds as the connect does.
net::coro::task<std::expected<net::dns::detail::resolution, std::error_code>>
joined(net::io::scheduler*                     sched,
       fake_network*                           network,
       test::blocking_lookup*                  lookup,
       const net::tcp_socket::connect_options& options,
       connecting*                             c)
{
    REQUIRE(sched->spawn(connect_named(sched, network, lookup, options, c)));

    const auto calls = co_await wait_calls(sched, lookup, 2);
    REQUIRE(calls);
    co_return lookup->resolver.start_resolving(sched, "host.test", net::protocol::not_care);
}

const net::ip_addr ipv6_1{
    net::ipv6_addr{0x2001, 0xdb8, 0, 0, 0, 0, 0, 1}
};
const net::ip_addr ipv4_1{
    net::ipv4_addr{192, 0, 2, 1}
};
const net::ip_addr ipv4_2{
    net::ipv4_addr{192, 0, 2, 2}
};

}

TEST_CASE("connecting starts at once on the host's IPv6 addresses", "[tcp][connect][happy-eyeballs][resolution]")
{
    test::blocking_lookup lookup;
    connecting            c; // what the scheduler's tasks write to

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // IPv6's are in first: the IPv6 address is tried while IPv4's are still to come (they're let through as it's
    // tried), and the IPv4 address once the first attempt has had the attempt delay.
    fake_network network{
        {{ipv6_1, hang}, {ipv4_1, answer}},
        listener->local_port()
    };
    lookup.answer = network.addresses_given();
    lookup.release(net::protocol::ipv6);
    network.on_attempt = [&lookup](std::size_t count)
    {
        if (count == 1) lookup.release(net::protocol::ipv4);
    };

    static constexpr auto delay = 100ms;

    sched.run_to_completion(connect_named(&sched, &network, &lookup, {.attempt_delay = delay}, &c));

    CAPTURE(c.err.message());
    REQUIRE_FALSE(c.err);

    const auto attempts = network.attempts();
    REQUIRE(attempts.size() == 2);
    CHECK(attempts[0].addr == ipv6_1);
    CHECK(attempts[1].addr == ipv4_1);
    CHECK(attempts[1].when - attempts[0].when >= delay - 10ms);
}

TEST_CASE("connecting waits the resolution delay for IPv6's addresses once IPv4's are in",
          "[tcp][connect][happy-eyeballs][resolution]")
{
    test::blocking_lookup lookup;
    connecting            c; // what the scheduler's tasks write to

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // IPv6's never come in (until the test's end): IPv4's are tried once they've waited the delay for them.
    fake_network network{
        {{ipv6_1, hang}, {ipv4_1, answer}},
        listener->local_port()
    };
    lookup.answer = network.addresses_given();
    lookup.release(net::protocol::ipv4);

    const net::tcp_socket::connect_options options{};
    REQUIRE(options.resolution_delay == 50ms); // RFC 8305's recommended

    // connects, then lets IPv6's call go (which the scheduler waits for).
    const auto test = [](net::io::scheduler*                     sched,
                         fake_network*                           network,
                         test::blocking_lookup*                  lookup,
                         const net::tcp_socket::connect_options* options,
                         connecting*                             c) -> net::coro::task<>
    {
        co_await connect_named(sched, network, lookup, *options, c);
        lookup->release();
    };

    const auto start = net::io::clock::now();
    sched.run_to_completion(test(&sched, &network, &lookup, &options, &c));

    CAPTURE(c.err.message());
    REQUIRE_FALSE(c.err);
    CHECK(c.elapsed < 5s);

    // (IPv4's came in after the start.)
    const auto attempts = network.attempts();
    REQUIRE(attempts.size() == 1);
    CHECK(attempts[0].addr == ipv4_1);
    CHECK(attempts[0].when - start >= options.resolution_delay);
}

TEST_CASE("connecting starts when IPv6's addresses come in within the resolution delay",
          "[tcp][connect][happy-eyeballs][resolution]")
{
    test::blocking_lookup lookup;
    connecting            c; // what the scheduler's tasks write to

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // a long delay, which IPv6's come in well within: once IPv4's are in (as the test's own resolution of the same
    // lookup sees). Then both are tried, in RFC 6724's order: IPv4's first, as the documentation's IPv6 address sorts
    // after it on any machine (see the resolver's tests).
    fake_network network{
        {{ipv6_1, hang}, {ipv4_1, answer}},
        listener->local_port()
    };
    lookup.answer = network.addresses_given();

    static constexpr auto delay = 10s;

    const auto test = [](net::io::scheduler* sched, fake_network* network, test::blocking_lookup* lookup, connecting* c)
        -> net::coro::task<std::pair<net::dns::detail::found, net::io::clock::time_point>>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        auto res = co_await joined(sched, network, lookup, {.timeout = 20s, .resolution_delay = delay}, c);
        REQUIRE(res.has_value());

        lookup->release(net::protocol::ipv4);
        auto found = co_await wait_found(sched, &*res, 0);

        const auto released = net::io::clock::now();
        lookup->release(net::protocol::ipv6);

        const auto done = co_await wait_done(sched, c);
        REQUIRE(done);
        co_return std::pair{std::move(found), released};
    };

    const auto [found, released] = sched.run_to_completion(test(&sched, &network, &lookup, &c));

    CAPTURE(c.err.message());
    CHECK_FALSE(c.err);

    // IPv4's were in first, alone.
    CHECK_FALSE(found.done);
    CHECK(found.family == net::protocol::ipv4);

    const auto attempts = network.attempts();
    REQUIRE(attempts.size() == 1);
    CHECK(attempts[0].addr == ipv4_1);
    CHECK(attempts[0].when >= released);
    CHECK(attempts[0].when - found.at < delay / 2);
}

TEST_CASE("connecting sorts a family's addresses that come in later into the attempts not yet made",
          "[tcp][connect][happy-eyeballs][resolution]")
{
    test::blocking_lookup lookup;
    connecting            c; // what the scheduler's tasks write to

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // IPv4's are in first, and tried after the resolution delay; IPv6's come in as the first is tried (waited for
    // there, through the test's own resolution of the same lookup, as the attempt delay is long), and the next attempt
    // is IPv6's (going on alternating by family), then the second IPv4 address's.
    fake_network network{
        {{ipv4_1, hang}, {ipv4_2, answer}, {ipv6_1, hang}},
        listener->local_port()
    };
    lookup.answer = network.addresses_given();

    std::atomic<const net::dns::detail::resolution*> watching{nullptr};
    std::atomic<bool>                                ipv6_in{false};
    network.on_attempt = [&](std::size_t count)
    {
        if (count != 1) return;

        // the first attempt is made: let IPv6's in, and wait (on the connecting thread) until the lookup's done.
        lookup.release(net::protocol::ipv6);
        const auto* res   = watching.load();
        const auto  until = net::io::clock::now() + 10s;
        while (res != nullptr && !res->look().done && net::io::clock::now() < until) std::this_thread::sleep_for(1ms);
        ipv6_in.store(res != nullptr && res->look().done);
    };

    static constexpr auto delay = 200ms;

    const auto test = [](net::io::scheduler*                               sched,
                         fake_network*                                     network,
                         test::blocking_lookup*                            lookup,
                         std::atomic<const net::dns::detail::resolution*>* watching,
                         connecting*                                       c) -> net::coro::task<>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        auto res = co_await joined(sched, network, lookup, {.attempt_delay = delay}, c);
        REQUIRE(res.has_value());
        watching->store(&*res);

        // NOTE: the hook's done with the resolution once connecting is (it's called by connecting's attempts).
        const net::util::defer unwatch{[watching] noexcept { watching->store(nullptr); }};

        lookup->release(net::protocol::ipv4);

        const auto done = co_await wait_done(sched, c);
        REQUIRE(done);
    };

    sched.run_to_completion(test(&sched, &network, &lookup, &watching, &c));

    CAPTURE(c.err.message());
    REQUIRE_FALSE(c.err);
    REQUIRE(ipv6_in.load());

    CHECK(network.tried() == std::vector{ipv4_1, ipv6_1, ipv4_2});

    const auto attempts = network.attempts();
    REQUIRE(attempts.size() == 3);
    CHECK(attempts[1].when - attempts[0].when >= delay - 10ms);
    CHECK(attempts[2].when - attempts[1].when >= delay - 10ms);
}

TEST_CASE("connecting waits for a family still to come once every attempt has failed",
          "[tcp][connect][happy-eyeballs][resolution]")
{
    test::blocking_lookup lookup;
    connecting            c; // what the scheduler's tasks write to

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    // IPv4's address is refused; IPv6's, which come in later, is unreachable: that's the error, so it was tried.
    fake_network network{
        {{ipv4_1, refuse}, {ipv6_1, unreachable}}
    };
    lookup.answer = network.addresses_given();

    static constexpr auto wait = 100ms;

    const auto test = [](net::io::scheduler* sched, fake_network* network, test::blocking_lookup* lookup, connecting* c)
        -> net::coro::task<std::uint64_t>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        REQUIRE(sched->spawn(connect_named(sched, network, lookup, {}, c)));

        lookup->release(net::protocol::ipv4);

        // the IPv4 attempt fails: connecting goes on waiting for IPv6's.
        const auto until = net::io::clock::now() + 10s;
        while (network->attempts().empty())
        {
            REQUIRE(net::io::clock::now() < until);
            const auto err = co_await sched->sleep_until(net::io::clock::now() + 1ms);
            REQUIRE_FALSE(err);
        }
        const auto slept = co_await sched->sleep_until(net::io::clock::now() + wait);
        REQUIRE_FALSE(slept);
        const auto done_then = c->done.load();

        lookup->release(net::protocol::ipv6);
        const auto done = co_await wait_done(sched, c);
        REQUIRE(done);
        co_return done_then;
    };

    const auto done_then = sched.run_to_completion(test(&sched, &network, &lookup, &c));

    CHECK(done_then == 0);
    CAPTURE(c.err.message());
    CHECK(c.err == std::errc::network_unreachable);
    CHECK(network.tried() == std::vector{ipv4_1, ipv6_1});
}

TEST_CASE("connecting tries a family that came in as the other's attempts failed",
          "[tcp][connect][happy-eyeballs][resolution]")
{
    test::blocking_lookup lookup;
    connecting            c; // what the scheduler's tasks write to

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // as with localhost and a server only on 127.0.0.1: IPv6's address is in first, and refused at once; IPv4's come
    // in as it's tried (the lookup is done by the time it has failed), and are tried then.
    fake_network network{
        {{ipv6_1, refuse}, {ipv4_1, answer}},
        listener->local_port()
    };
    lookup.answer = network.addresses_given();

    std::atomic<const net::dns::detail::resolution*> watching{nullptr};
    std::atomic<bool>                                ipv4_in{false};
    network.on_attempt = [&](std::size_t count)
    {
        if (count != 1) return;

        // the first attempt is made: let IPv4's in, and wait (on the connecting thread) until the lookup's done.
        lookup.release(net::protocol::ipv4);
        const auto* res   = watching.load();
        const auto  until = net::io::clock::now() + 10s;
        while (res != nullptr && !res->look().done && net::io::clock::now() < until) std::this_thread::sleep_for(1ms);
        ipv4_in.store(res != nullptr && res->look().done);
    };

    const auto test = [](net::io::scheduler*                               sched,
                         fake_network*                                     network,
                         test::blocking_lookup*                            lookup,
                         std::atomic<const net::dns::detail::resolution*>* watching,
                         connecting*                                       c) -> net::coro::task<>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        auto res = co_await joined(sched, network, lookup, {}, c);
        REQUIRE(res.has_value());
        watching->store(&*res);

        // NOTE: the hook's done with the resolution once connecting is (it's called by connecting's attempts).
        const net::util::defer unwatch{[watching] noexcept { watching->store(nullptr); }};

        lookup->release(net::protocol::ipv6);

        const auto done = co_await wait_done(sched, c);
        REQUIRE(done);
    };

    sched.run_to_completion(test(&sched, &network, &lookup, &watching, &c));

    REQUIRE(ipv4_in.load());
    CAPTURE(c.err.message());
    CHECK_FALSE(c.err);
    CHECK(network.tried() == std::vector{ipv6_1, ipv4_1});
}

TEST_CASE("connecting goes on if one family fails to resolve", "[tcp][connect][happy-eyeballs][resolution]")
{
    test::blocking_lookup lookup;
    lookup.release();
    connecting c; // what the scheduler's tasks write to

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto closed = static_cast<std::uint16_t>(std::stoi(closed_port()));

    SECTION("IPv6's")
    {
        auto listener = net::listener::make(&sched, "127.0.0.1", "0");
        REQUIRE(listener.has_value());

        // the name has no IPv6 addresses.
        fake_network network{{{ipv4_1, answer}}, listener->local_port()};
        lookup.answer = network.addresses_given();

        sched.run_to_completion(connect_named(&sched, &network, &lookup, {}, &c));

        CAPTURE(c.err.message());
        CHECK_FALSE(c.err);
        CHECK(network.tried() == std::vector{ipv4_1});
    }

    SECTION("IPv4's")
    {
        // the name has no IPv4 addresses: the IPv6 address is tried (and fails).
        fake_network network{{{ipv6_1, refuse}}, 0, closed};
        lookup.answer = network.addresses_given();

        sched.run_to_completion(connect_named(&sched, &network, &lookup, {}, &c));

        CHECK(c.err == std::errc::connection_refused);
        CHECK(network.tried() == std::vector{ipv6_1});
    }

    CHECK(lookup.started.load() == 2);
}

TEST_CASE("connecting fails with the resolver's error if both families fail to resolve",
          "[tcp][connect][happy-eyeballs][resolution]")
{
    struct test_case
    {
        int ipv6     = 0;
        int ipv4     = 0;
        int expected = 0;
    };

    // as the resolver chooses (see its tests): a temporary failure, if only one is, otherwise IPv4's.
    auto tc = GENERATE(test_case{.ipv6 = EAI_NONAME, .ipv4 = EAI_NONAME, .expected = EAI_NONAME},
                       test_case{.ipv6 = EAI_AGAIN, .ipv4 = EAI_NONAME, .expected = EAI_AGAIN},
                       test_case{.ipv6 = EAI_NONAME, .ipv4 = EAI_FAIL, .expected = EAI_FAIL});
    CAPTURE(tc.ipv6, tc.ipv4);

    net::dns::resolver resolver{
        {
         .use           = net::dns::resolver::backend::system,
         .system_lookup = [tc](const std::string& /*host*/,
         net::protocol proto) -> std::expected<std::vector<net::ip_addr>, std::error_code>
            { return std::unexpected(net::make_gai_error_code(proto == net::protocol::ipv6 ? tc.ipv6 : tc.ipv4)); },
         }
    };

    fake_network       network{{}};
    net::io::scheduler sched{make_pool()};

    const auto connect = [](net::io::scheduler* sched,
                            net::dns::resolver* resolver,
                            fake_network*       network) -> net::coro::task<std::error_code>
    {
        auto conn = co_await net::tcp_socket::connect(sched,
                                                      "nowhere.test",
                                                      "80",
                                                      {.resolver = resolver, .system_connect = network->hook()});
        co_return conn.has_value() ? std::error_code{} : conn.error();
    };

    const auto err = sched.run_to_completion(connect(&sched, &resolver, &network));
    CAPTURE(err.message());
    CHECK(err == net::make_gai_error_code(tc.expected));
    CHECK(network.tried().empty());
}

TEST_CASE("connecting ends at its deadline or when stopped during the resolution delay",
          "[tcp][connect][happy-eyeballs][resolution][timeout][stop]")
{
    test::blocking_lookup lookup;
    connecting            c; // what the scheduler's tasks write to

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    // IPv4's are in, and wait for IPv6's (which never come) for a long delay, which the deadline or a stop ends first.
    fake_network network{
        {{ipv6_1, hang}, {ipv4_1, hang}}
    };
    lookup.answer = network.addresses_given();

    static constexpr auto delay = 10s;

    SECTION("the deadline")
    {
        static constexpr auto timeout = 200ms;

        const auto test = [](net::io::scheduler*    sched,
                             fake_network*          network,
                             test::blocking_lookup* lookup,
                             connecting*            c) -> net::coro::task<net::dns::detail::found>
        {
            // lets its calls go however this ends (the scheduler waits for them).
            const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

            auto res = co_await joined(sched,
                                       network,
                                       lookup,
                                       {.timeout = timeout, .attempt_delay = 50ms, .resolution_delay = delay},
                                       c);
            REQUIRE(res.has_value());

            lookup->release(net::protocol::ipv4);
            auto found = co_await wait_found(sched, &*res, 0);

            const auto done = co_await wait_done(sched, c);
            REQUIRE(done);
            co_return found;
        };

        const auto found = sched.run_to_completion(test(&sched, &network, &lookup, &c));

        CHECK(found.family == net::protocol::ipv4);
        CHECK(c.err == std::errc::timed_out);
        CHECK(c.elapsed >= timeout);
        CHECK(c.elapsed < 5s);
        CHECK(network.tried().empty());
    }

    SECTION("a stop")
    {
        std::stop_source source;

        const auto test = [](net::io::scheduler*    sched,
                             fake_network*          network,
                             test::blocking_lookup* lookup,
                             std::stop_source*      source,
                             connecting*            c) -> net::coro::task<net::dns::detail::found>
        {
            // lets its calls go however this ends (the scheduler waits for them).
            const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

            auto res = co_await joined(
                sched,
                network,
                lookup,
                {.timeout = 20s, .attempt_delay = 50ms, .stop = source->get_token(), .resolution_delay = delay},
                c);
            REQUIRE(res.has_value());

            lookup->release(net::protocol::ipv4);
            auto found = co_await wait_found(sched, &*res, 0);
            source->request_stop();

            const auto done = co_await wait_done(sched, c);
            REQUIRE(done);
            co_return found;
        };

        const auto found = sched.run_to_completion(test(&sched, &network, &lookup, &source, &c));

        CHECK(found.family == net::protocol::ipv4);
        CHECK(c.err == std::errc::operation_canceled);
        CHECK(c.elapsed < 5s);
        CHECK(network.tried().empty());
    }
}

TEST_CASE("an attempt delay of 0 waits for both families' addresses", "[tcp][connect][happy-eyeballs][resolution]")
{
    test::blocking_lookup lookup;
    connecting            c; // what the scheduler's tasks write to

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // IPv4's are in, but nothing's tried until IPv6's are too; then they're tried in RFC 6724's order (IPv4's first).
    fake_network network{
        {{ipv6_1, refuse}, {ipv4_1, answer}},
        listener->local_port()
    };
    lookup.answer = network.addresses_given();

    static constexpr auto wait = 200ms;

    const auto test = [](net::io::scheduler* sched, fake_network* network, test::blocking_lookup* lookup, connecting* c)
        -> net::coro::task<std::pair<bool, net::io::clock::time_point>>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        REQUIRE(sched->spawn(connect_named(sched, network, lookup, {.attempt_delay = 0ms}, c)));

        lookup->release(net::protocol::ipv4);
        const auto slept = co_await sched->sleep_until(net::io::clock::now() + wait);
        REQUIRE_FALSE(slept);
        const bool tried_then = !network->attempts().empty();

        const auto released = net::io::clock::now();
        lookup->release(net::protocol::ipv6);

        const auto done = co_await wait_done(sched, c);
        REQUIRE(done);
        co_return std::pair{tried_then, released};
    };

    const auto [tried_then, released] = sched.run_to_completion(test(&sched, &network, &lookup, &c));

    CAPTURE(c.err.message());
    CHECK_FALSE(c.err);
    CHECK_FALSE(tried_then);

    const auto attempts = network.attempts();
    REQUIRE(attempts.size() == 1);
    CHECK(attempts[0].addr == ipv4_1);
    CHECK(attempts[0].when >= released);
}
