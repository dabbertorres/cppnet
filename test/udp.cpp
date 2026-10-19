#include "udp.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"

namespace
{

using namespace std::chrono_literals;

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(2); }

std::span<const std::byte> bytes(std::string_view str) { return std::as_bytes(std::span{str}); }

std::string text(std::span<const std::byte> data) { return {reinterpret_cast<const char*>(data.data()), data.size()}; }

// received is what a socket received: the text, and who sent it (or why it failed).
struct received
{
    std::string     text{};
    net::ip_addr    addr{};
    std::uint16_t   port = 0;
    std::error_code err{};
};

net::coro::task<received> receive(net::udp_socket* sock)
{
    std::array<std::byte, 64> buf{};

    auto got = co_await sock->receive_from(buf);
    if (!got.has_value()) co_return received{.text = {}, .addr = {}, .port = 0, .err = got.error()};

    co_return received{.text = text(std::span{buf}.first(got->size)), .addr = got->addr, .port = got->port, .err = {}};
}

net::coro::task<std::expected<std::size_t, std::error_code>>
send(net::udp_socket* sock, std::string_view str, net::ip_addr addr, std::uint16_t port)
{
    co_return co_await sock->send_to(bytes(str), addr, port);
}

// read reads a datagram with read(), as a connected socket would.
net::coro::task<received> read(net::udp_socket* sock)
{
    std::array<std::byte, 64> buf{};

    auto res = co_await sock->read(buf);
    co_return received{.text = text(std::span{buf}.first(res.count)), .addr = {}, .port = 0, .err = res.err};
}

net::coro::task<std::error_code> write(net::udp_socket* sock, std::string_view str)
{
    auto res = co_await sock->write(bytes(str));
    co_return res.err;
}

// bind_or_skip binds a socket to addr, skipping the test if it can't (e.g. ::1, in a container without IPv6).
net::udp_socket bind_or_skip(net::io::scheduler* sched, const net::ip_addr& addr)
{
    auto sock = net::udp_socket::bind(sched, addr, 0);
    if (!sock.has_value()) SKIP("can't bind to " + addr.to_string() + ": " + sock.error().message());
    return std::move(*sock);
}

}

TEST_CASE("a connected socket exchanges datagrams with a bound one", "[udp]")
{
    net::io::scheduler sched{make_pool()};

    const auto loopback = GENERATE(net::ip_addr{net::ipv4_addr::loopback()}, net::ip_addr{net::ipv6_addr::loopback()});
    CAPTURE(loopback.to_string());

    auto server = bind_or_skip(&sched, loopback);
    REQUIRE(server.local_port() != 0);

    auto client = net::udp_socket::connect(&sched, loopback, server.local_port());
    REQUIRE(client.has_value());
    client->set_timeout(10s);
    server.set_timeout(10s);

    struct exchange
    {
        std::error_code                             write_err{};
        received                                    ping{};
        std::expected<std::size_t, std::error_code> sent = 0;
        received                                    pong{};
    };

    const auto run = [](net::udp_socket* client, net::udp_socket* server) -> net::coro::task<exchange>
    {
        exchange out;
        out.write_err = co_await write(client, "ping");

        // the server sees who sent it, and answers there.
        out.ping = co_await receive(server);
        out.sent = co_await send(server, "pong", out.ping.addr, out.ping.port);
        out.pong = co_await read(client);
        co_return out;
    };

    auto out = sched.run_to_completion(run(&*client, &server));

    CAPTURE(out.write_err.message(), out.ping.err.message(), out.pong.err.message());
    CHECK_FALSE(out.write_err);

    CHECK_FALSE(out.ping.err);
    CHECK(out.ping.text == "ping");
    CHECK(out.ping.addr == loopback);
    CHECK(out.ping.port == client->local_port());

    CHECK(out.sent == 4);

    CHECK_FALSE(out.pong.err);
    CHECK(out.pong.text == "pong");
}

TEST_CASE("bound sockets exchange datagrams with send_to and receive_from", "[udp]")
{
    net::io::scheduler sched{make_pool()};

    const net::ip_addr loopback{net::ipv4_addr::loopback()};

    auto first  = bind_or_skip(&sched, loopback);
    auto second = bind_or_skip(&sched, loopback);
    first.set_timeout(10s);
    second.set_timeout(10s);

    struct exchange
    {
        received there{};
        received back{};
    };

    const auto run =
        [](net::udp_socket* first, net::udp_socket* second, net::ip_addr loopback) -> net::coro::task<exchange>
    {
        exchange out;

        auto sent = co_await send(first, "there", loopback, second->local_port());
        if (!sent.has_value()) out.there.err = sent.error();
        else out.there = co_await receive(second);

        sent = co_await send(second, "back", out.there.addr, out.there.port);
        if (!sent.has_value()) out.back.err = sent.error();
        else out.back = co_await receive(first);

        co_return out;
    };

    auto out = sched.run_to_completion(run(&first, &second, loopback));

    CAPTURE(out.there.err.message(), out.back.err.message());
    CHECK_FALSE(out.there.err);
    CHECK(out.there.text == "there");
    CHECK(out.there.port == first.local_port());

    CHECK_FALSE(out.back.err);
    CHECK(out.back.text == "back");
    CHECK(out.back.addr == loopback);
    CHECK(out.back.port == second.local_port());
}

TEST_CASE("a connected socket only reads its peer's datagrams", "[udp]")
{
    net::io::scheduler sched{make_pool()};

    const net::ip_addr loopback{net::ipv4_addr::loopback()};

    auto server   = bind_or_skip(&sched, loopback);
    auto stranger = bind_or_skip(&sched, loopback);

    auto client = net::udp_socket::connect(&sched, loopback, server.local_port());
    REQUIRE(client.has_value());
    client->set_timeout(10s);

    const auto run =
        [](net::udp_socket* client, net::udp_socket* server, net::udp_socket* stranger) -> net::coro::task<received>
    {
        const auto client_port = client->local_port();

        // the stranger's comes first, and isn't read.
        auto sent = co_await send(stranger, "stranger", net::ipv4_addr::loopback(), client_port);
        if (!sent.has_value()) co_return received{.text = {}, .addr = {}, .port = 0, .err = sent.error()};

        sent = co_await send(server, "server", net::ipv4_addr::loopback(), client_port);
        if (!sent.has_value()) co_return received{.text = {}, .addr = {}, .port = 0, .err = sent.error()};

        co_return co_await receive(client);
    };

    auto got = sched.run_to_completion(run(&*client, &server, &stranger));

    CAPTURE(got.err.message());
    CHECK_FALSE(got.err);
    CHECK(got.text == "server");
    CHECK(got.port == server.local_port());
}

TEST_CASE("an empty datagram is received as one", "[udp]")
{
    net::io::scheduler sched{make_pool()};

    const net::ip_addr loopback{net::ipv4_addr::loopback()};

    auto sender   = bind_or_skip(&sched, loopback);
    auto receiver = bind_or_skip(&sched, loopback);
    receiver.set_timeout(10s);

    const auto run = [](net::udp_socket* sender, net::udp_socket* receiver) -> net::coro::task<received>
    {
        auto sent = co_await send(sender, "", net::ipv4_addr::loopback(), receiver->local_port());
        if (!sent.has_value()) co_return received{.text = {}, .addr = {}, .port = 0, .err = sent.error()};
        if (*sent != 0) co_return received{.text = "sent " + std::to_string(*sent), .addr = {}, .port = 0, .err = {}};

        co_return co_await receive(receiver);
    };

    auto got = sched.run_to_completion(run(&sender, &receiver));

    CAPTURE(got.err.message());
    CHECK_FALSE(got.err);
    CHECK(got.text.empty());
    CHECK(got.port == sender.local_port());
}

TEST_CASE("receiving gives up at the socket's timeout", "[udp][timeout]")
{
    net::io::scheduler sched{make_pool()};

    auto sock = bind_or_skip(&sched, net::ipv4_addr::loopback());
    sock.set_read_timeout(50ms);

    auto got = sched.run_to_completion(receive(&sock));
    CHECK(got.err == std::errc::timed_out);
}

TEST_CASE("a connected socket's datagram to a closed port is refused", "[udp]")
{
    net::io::scheduler sched{make_pool()};

    // bind a port, and then let it go: nothing is bound to it afterwards.
    std::uint16_t port = 0;
    {
        auto closed = bind_or_skip(&sched, net::ipv4_addr::loopback());
        port        = closed.local_port();
    }

    auto client = net::udp_socket::connect(&sched, net::ipv4_addr::loopback(), port);
    REQUIRE(client.has_value());
    client->set_timeout(10s);

    // the ICMP refusal of what it wrote fails the read.
    const auto run = [](net::udp_socket* client) -> net::coro::task<received>
    {
        if (auto err = co_await write(client, "anyone?"); err)
            co_return received{.text = {}, .addr = {}, .port = 0, .err = err};

        co_return co_await read(client);
    };

    auto got = sched.run_to_completion(run(&*client));
    CHECK(got.err == std::errc::connection_refused);
}
