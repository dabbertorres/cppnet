#include "socket.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "tcp.hpp"
#include "util/defer.hpp"

#ifdef __linux__
#    include <sys/ioctl.h>

#    include <linux/sockios.h>
#endif

namespace
{

using namespace std::chrono_literals;

using net::coro::task;
using net::io::scheduler;

constexpr auto test_timeout = 5s;

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(2); }

// eventually polls until check passes, or the test times out.
template<typename F>
bool eventually(F check)
{
    auto give_up_at = std::chrono::steady_clock::now() + test_timeout;
    while (!check())
    {
        if (std::chrono::steady_clock::now() > give_up_at) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// connection is a TCP connection over loopback: the writer's side, whose writability is tested, and its peer's, which
// reads only when a test says (see drain()).
struct connection
{
    net::tcp_socket writer;
    net::tcp_socket peer;
};

// connect_loopback connects, with small buffers (so that filling them is quick, and doesn't depend on the system's
// defaults), and TCP_NODELAY on the writer's side, as the HTTP server sets it.
connection connect_loopback(scheduler* sched)
{
    const int listening = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listening != -1);
    const net::util::defer close_listening{[listening] noexcept { ::close(listening); }};

    // the peer's receive buffer: the listener's (set before connecting, as TCP's window scale is agreed then).
    constexpr int receive_buffer = 16 * 1'024;
    REQUIRE(::setsockopt(listening, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) == 0);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(listening, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(listening, 1) == 0);

    socklen_t size = sizeof(addr);
    REQUIRE(::getsockname(listening, reinterpret_cast<sockaddr*>(&addr), &size) == 0);

    const int writing = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(writing != -1);
    net::tcp_socket writer{sched, writing};

    constexpr int send_buffer = 64 * 1'024;
    REQUIRE(::setsockopt(writing, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    REQUIRE(writer.set_nodelay().has_value());

    // NOTE: the socket's non-blocking (the writer made it so), but the listener isn't: once it's accepted, the
    //       connection's made.
    if (::connect(writing, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) REQUIRE(errno == EINPROGRESS);

    const int accepted = ::accept(listening, nullptr, nullptr);
    REQUIRE(accepted != -1);

    return {
        .writer = std::move(writer),
        .peer   = net::tcp_socket{sched, accepted}
    };
}

// writable is whether poll() says the socket is writable.
bool writable(const net::socket& sock)
{
    pollfd entry{.fd = sock.native_handle(), .events = POLLOUT, .revents = 0};

    const int num = ::poll(&entry, 1, 0);
    REQUIRE(num != -1);
    return num > 0;
}

// held is how much the TCP socket holds that its peer hasn't acknowledged: sent, or not yet.
std::int64_t held(const net::socket& sock)
{
    int value = 0;
#ifdef __linux__
    REQUIRE(::ioctl(sock.native_handle(), SIOCOUTQ, &value) == 0);
#elifdef __APPLE__
    socklen_t size = sizeof(value);
    REQUIRE(::getsockopt(sock.native_handle(), SOL_SOCKET, SO_NWRITE, &value, &size) == 0);
#else
    SKIP("no way to tell what a socket holds here");
#endif
    return value;
}

// in_flight is how much the TCP socket has sent that its peer hasn't acknowledged, given all that's been written to it.
// NOTE: from counters read one after another, while the kernel may be sending: a snapshot that's off only puts the
//       answer off, which fill() then asks again.
std::int64_t in_flight(const net::socket& sock, [[maybe_unused]] std::size_t written)
{
#ifdef __linux__
    // what's held, less what's not sent.
    int unsent = 0;
    REQUIRE(::ioctl(sock.native_handle(), SIOCOUTQNSD, &unsent) == 0);
    return held(sock) - unsent;
#elifdef __APPLE__
    // what's been sent (but again), less what's acknowledged: what's written, less what's held.
    tcp_connection_info info{};
    socklen_t           size = sizeof(info);
    REQUIRE(::getsockopt(sock.native_handle(), IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &size) == 0);

    const auto sent  = static_cast<std::int64_t>(info.tcpi_txbytes - info.tcpi_txretransmitbytes);
    const auto acked = static_cast<std::int64_t>(written) - held(sock);
    return sent - acked;
#else
    SKIP("no way to tell what's in flight here");
    return 0;
#endif
}

// fill writes to the TCP socket until it isn't writable, and stays so until its peer reads: until poll() says it isn't
// writable (the send buffer is full, or what's not sent is past the TCP_NOTSENT_LOWAT mark, if one's set) and nothing's
// in flight, so the peer's window is closed (or too small to send into, as macOS's sender has it), and what's not sent
// stays so. Stopping as soon as poll() says it isn't writable would leave it to what the kernel's still doing: over
// macOS's loopback, which delivers apart from send(), an acknowledgement still to come opening the window made it
// writable again in about one try in twenty.
std::size_t fill(const net::socket& sock)
{
    constexpr std::size_t        most = 10'000;
    const std::array<char, 4096> chunk{};

    std::size_t written     = 0;
    const auto  write_while = [&]
    {
        for (std::size_t i = 0; i < most && writable(sock); ++i)
        {
            const auto sent = ::send(sock.native_handle(), chunk.data(), chunk.size(), 0);
            if (sent >= 0) written += static_cast<std::size_t>(sent);
            else REQUIRE((errno == EAGAIN || errno == EWOULDBLOCK));
        }
    };

    REQUIRE(eventually(
        [&]
        {
            write_while();
            return in_flight(sock, written) == 0 && !writable(sock);
        }));

    return written;
}

// send_buffer is the socket's send buffer's size, as the kernel reports it.
std::int64_t send_buffer(const net::socket& sock)
{
    int       value = 0;
    socklen_t size  = sizeof(value);
    REQUIRE(::getsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDBUF, &value, &size) == 0);
    return value;
}

// drain reads what's arrived at the socket, without waiting.
void drain(const net::socket& sock)
{
    std::array<char, std::size_t{16} * 1'024> buf{};
    while (::recv(sock.native_handle(), buf.data(), buf.size(), 0) > 0)
    {}
}

task<net::io::result> wait_writable(net::socket* sock) { co_return co_await sock->wait_writable(); }

task<net::io::result> write(net::socket* sock, std::span<const std::byte> data)
{
    co_return co_await sock->write(data);
}

}

TEST_CASE("wait_writable returns at once on a socket that's writable", "[socket][wait-writable]")
{
    scheduler sched{make_pool()};
    auto [writer, peer] = connect_loopback(&sched);

    const auto mark = GENERATE(std::size_t{0}, std::size_t{4096});
    CAPTURE(mark);
    if (mark != 0) REQUIRE(writer.set_notsent_lowat(mark).has_value());

    // without a coroutine.
    auto waiting = writer.wait_writable();
    REQUIRE(waiting.finished_without_coroutine());

    auto res = std::move(waiting).result();
    CHECK_FALSE(res.err);
    CHECK(res.count == 0);
}

TEST_CASE("wait_writable on a closed socket fails at once", "[socket][wait-writable]")
{
    scheduler       sched{make_pool()};
    net::tcp_socket closed{&sched};

    auto res = closed.wait_writable().result();
    CHECK(res.err == std::errc::bad_file_descriptor);
}

TEST_CASE("wait_writable waits until the peer has read", "[socket][wait-writable]")
{
    scheduler sched{make_pool()};
    auto [writer, peer] = connect_loopback(&sched);

    // without a mark, until the send buffer has room; with one, until what's not sent is under it (Linux: under half).
    const auto mark = GENERATE(std::size_t{0}, std::size_t{4096});
    CAPTURE(mark);
    if (mark != 0) REQUIRE(writer.set_notsent_lowat(mark).has_value());

    fill(writer);

    // with a mark, it isn't writable though its send buffer has room: on Linux, send() stops at the mark; on macOS, it
    // would still take more, but poll() says the socket isn't writable.
    if (mark != 0) CHECK(held(writer) < send_buffer(writer) / 2);

    // nothing reads, so it waits: resumed here, it waits once this returns, and nothing but a read can end that.
    auto waiting = wait_writable(&writer);
    waiting.get_handle().resume();
    REQUIRE_FALSE(waiting.get_promise().finished());
    REQUIRE_FALSE(writable(writer));

    // then the peer reads, and the kernel sends the rest, and reports the socket writable.
    REQUIRE(eventually(
        [&, &peer = peer]
        {
            drain(peer);
            return waiting.get_promise().finished();
        }));

    auto res = std::move(waiting).result();
    CHECK_FALSE(res.err);
    CHECK(res.count == 0);

    // nothing's been written since, so it still is.
    CHECK(writable(writer));
}

TEST_CASE("wait_writable ends at the socket's write deadline or timeout", "[socket][wait-writable][timeout]")
{
    scheduler sched{make_pool()};
    auto [writer, peer] = connect_loopback(&sched);

    REQUIRE(writer.set_notsent_lowat(4096).has_value());

    constexpr auto limit = 100ms;
    const auto     start = net::io::clock::now();

    SECTION("the deadline")
    {
        writer.set_write_deadline(net::io::clock::now() + limit);
    }
    SECTION("the timeout")
    {
        writer.set_write_timeout(limit);
    }

    fill(writer);

    auto       res     = sched.run_to_completion(wait_writable(&writer));
    const auto elapsed = net::io::clock::now() - start;

    CHECK(res.err == std::errc::timed_out);
    CHECK(res.count == 0);
    CHECK(elapsed >= limit);
    CHECK(elapsed < test_timeout);
}

TEST_CASE("wait_writable ends when the socket's stop token is stopped", "[socket][wait-writable][stop]")
{
    std::stop_source source;
    scheduler        sched{make_pool()};
    auto [writer, peer] = connect_loopback(&sched);
    writer.set_stop_token(source.get_token());
    REQUIRE(writer.set_notsent_lowat(4096).has_value());

    fill(writer);

    auto waiting = wait_writable(&writer);
    waiting.get_handle().resume();
    REQUIRE_FALSE(waiting.get_promise().finished());

    source.request_stop();
    REQUIRE(eventually([&] { return waiting.get_promise().finished(); }));

    auto res = std::move(waiting).result();
    CHECK(res.err == std::errc::operation_canceled);
    CHECK(res.count == 0);
}

TEST_CASE("wait_writable can't wait alongside a write", "[socket][wait-writable]")
{
    scheduler sched{make_pool()};
    auto [writer, peer] = connect_loopback(&sched);

    fill(writer);

    // more than the buffers hold, so a write of it waits.
    const std::vector<std::byte> lots(std::size_t{1'024} * 1'024);

    const auto drain_until = [&peer = peer](const task<net::io::result>& waiting)
    {
        return eventually(
            [&]
            {
                drain(peer);
                return waiting.get_promise().finished();
            });
    };

    SECTION("while it waits")
    {
        auto waiting = wait_writable(&writer);
        waiting.get_handle().resume();
        REQUIRE_FALSE(waiting.get_promise().finished());

        // a write that has to wait fails at once (having written what it could without waiting), as does a second wait.
        auto writing = write(&writer, lots);
        writing.get_handle().resume();
        REQUIRE(writing.get_promise().finished());
        const auto wrote = std::move(writing).result();
        CHECK(wrote.err == std::errc::device_or_resource_busy);

        auto second = writer.wait_writable();
        REQUIRE(second.finished_without_coroutine());
        const auto waited = std::move(second).result();
        CHECK(waited.err == std::errc::device_or_resource_busy);

        // and the wait goes on, until the peer has read.
        REQUIRE(drain_until(waiting));
        const auto res = std::move(waiting).result();
        CHECK_FALSE(res.err);
    }

    SECTION("while a write waits")
    {
        auto writing = write(&writer, lots);
        writing.get_handle().resume();
        REQUIRE_FALSE(writing.get_promise().finished());

        auto waiting = writer.wait_writable();
        REQUIRE(waiting.finished_without_coroutine());
        const auto waited = std::move(waiting).result();
        CHECK(waited.err == std::errc::device_or_resource_busy);

        // and the write goes on, until the peer has read it all.
        REQUIRE(drain_until(writing));
        auto res = std::move(writing).result();
        CHECK_FALSE(res.err);
        CHECK(res.count == lots.size());
    }
}
