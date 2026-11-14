#include "listen.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "exception.hpp"
#include "io/scheduler.hpp"
#include "tcp.hpp"
#include "util/defer.hpp"

namespace
{

using namespace std::chrono_literals;

using accepted_t = std::expected<net::tcp_socket, std::error_code>;

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

// connect_to connects to port on 127.0.0.1 with a blocking POSIX socket (so without the scheduler), returning it: it's
// connected once the kernel has the connection queued for the listener.
int connect_to(std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd != -1);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        FAIL("failed to connect");
    }
    return fd;
}

// waiting_connection is whether a connection is queued for the listener, which waits up to the test's timeout for one.
bool waiting_connection(const net::listener& listener)
{
    pollfd entry{.fd = listener.native_handle(), .events = POLLIN, .revents = 0};
    return ::poll(&entry, 1, static_cast<int>(std::chrono::milliseconds{test_timeout}.count())) == 1;
}

// accept_one awaits an accept: resumed on the test's thread, it's waiting (if it has to) once that returns.
net::coro::task<accepted_t> accept_one(net::listener* listener) { co_return co_await listener->accept(); }

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

TEST_CASE("listener::accept takes a connection that's waiting without a coroutine", "[listener][accept]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    const int              client = connect_to(listener->local_port());
    const net::util::defer close_client{[client] noexcept { ::close(client); }};
    REQUIRE(waiting_connection(*listener));

    auto accepting = listener->accept();
    REQUIRE(accepting.finished_without_coroutine());

    auto accepted = std::move(accepting).result();
    REQUIRE(accepted.has_value());
    CHECK(accepted->valid());
}

TEST_CASE("listener::accept waits for a connection, one accept at a time", "[listener][accept]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // nothing connects, so it waits: resumed here, it waits once this returns.
    auto waiting = accept_one(&*listener);
    waiting.get_handle().resume();
    REQUIRE_FALSE(waiting.get_promise().finished());

    // another that would have to wait fails at once.
    auto second = listener->accept();
    REQUIRE(second.finished_without_coroutine());
    const auto busy = std::move(second).result();
    REQUIRE_FALSE(busy.has_value());
    CHECK(busy.error() == std::errc::device_or_resource_busy);

    // the first is still waiting, and takes the next connection.
    const int              client = connect_to(listener->local_port());
    const net::util::defer close_client{[client] noexcept { ::close(client); }};
    REQUIRE(eventually([&] { return waiting.get_promise().finished(); }));

    auto accepted = std::move(waiting).result();
    REQUIRE(accepted.has_value());
    CHECK(accepted->valid());

    // and then the listener's free for the next accept to wait.
    auto next = accept_one(&*listener);
    next.get_handle().resume();
    REQUIRE_FALSE(next.get_promise().finished());

    const int              another = connect_to(listener->local_port());
    const net::util::defer close_another{[another] noexcept { ::close(another); }};
    REQUIRE(eventually([&] { return next.get_promise().finished(); }));
    const auto taken = std::move(next).result();
    CHECK(taken.has_value());
}

TEST_CASE("listener::accept waiting ends when the listener's shut down", "[listener][accept]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    auto waiting = accept_one(&*listener);
    waiting.get_handle().resume();
    REQUIRE_FALSE(waiting.get_promise().finished());

    listener->shutdown();
    REQUIRE(eventually([&] { return waiting.get_promise().finished(); }));

    const auto res = std::move(waiting).result();
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == std::errc::operation_canceled);

    // and so does an accept after it, at once.
    auto after = listener->accept();
    REQUIRE(after.finished_without_coroutine());
    const auto later = std::move(after).result();
    REQUIRE_FALSE(later.has_value());
    CHECK(later.error() == std::errc::operation_canceled);
}

TEST_CASE("listener::accept waiting ends when the scheduler's stopped", "[listener][accept]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    auto waiting = accept_one(&*listener);
    waiting.get_handle().resume();
    REQUIRE_FALSE(waiting.get_promise().finished());

    sched.request_stop();
    REQUIRE(eventually([&] { return waiting.get_promise().finished(); }));

    const auto res = std::move(waiting).result();
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == std::errc::operation_canceled);
}

TEST_CASE("listener::accept waiting is canceled with the coroutine awaiting it", "[listener][accept]")
{
    net::io::scheduler sched{make_pool()};

    auto listener = net::listener::make(&sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());

    // destroyed while it waits (nothing connects, so nothing resumes it meanwhile).
    {
        auto waiting = accept_one(&*listener);
        waiting.get_handle().resume();
        REQUIRE_FALSE(waiting.get_promise().finished());
    }

    // which leaves neither the listener's accept nor the scheduler's wait taken: the next accept waits, and takes the
    // next connection.
    auto next = accept_one(&*listener);
    next.get_handle().resume();
    REQUIRE_FALSE(next.get_promise().finished());

    const int              client = connect_to(listener->local_port());
    const net::util::defer close_client{[client] noexcept { ::close(client); }};
    REQUIRE(eventually([&] { return next.get_promise().finished(); }));
    const auto taken = std::move(next).result();
    CHECK(taken.has_value());
}
