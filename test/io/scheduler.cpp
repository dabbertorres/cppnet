#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "socket.hpp"
#include "tcp.hpp"

namespace
{

using namespace std::chrono_literals;
using namespace std::string_view_literals;

using net::coro::task;
using net::io::scheduler;

constexpr auto test_timeout = 5s;

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

std::pair<net::socket, net::socket> make_socket_pair(scheduler* sched)
{
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    return {net::socket{sched, fds[0]}, net::socket{sched, fds[1]}};
}

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

task<net::io::result> read_some(net::socket* sock, std::string* out)
{
    std::array<std::byte, 256> buf{};

    auto res = co_await sock->read(buf);
    out->append(reinterpret_cast<const char*>(buf.data()), res.count);
    co_return res;
}

// read_until_canceled reads until it fails, and then records how (and that it got to run code afterwards).
task<> read_until_failure(net::socket* sock, std::error_condition* err, std::atomic<bool>* finished)
{
    std::string ignored;

    while (true)
    {
        auto res = co_await read_some(sock, &ignored);
        if (res.err)
        {
            *err = res.err;
            break;
        }
    }

    finished->store(true);
}

std::string local_port(const net::listener& listener)
{
    sockaddr_storage addr{};
    socklen_t        size = sizeof(addr);
    REQUIRE(::getsockname(listener.native_handle(), reinterpret_cast<sockaddr*>(&addr), &size) == 0);

    if (addr.ss_family == AF_INET6) return std::to_string(ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port));
    return std::to_string(ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port));
}

}

TEST_CASE("reads wait until data arrives", "[io][scheduler]")
{
    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    std::thread later{[fd = writer.native_handle()]
                      {
                          std::this_thread::sleep_for(50ms);
                          ::send(fd, "hello", 5, 0);
                      }};

    std::string received;
    auto        res = sched.run_to_completion(read_some(&reader, &received));
    later.join();

    CHECK_FALSE(res.err);
    CHECK(received == "hello");
}

TEST_CASE("reads report the other end closing", "[io][scheduler]")
{
    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    writer.close(false);

    std::string received;
    auto        res = sched.run_to_completion(read_some(&reader, &received));

    CHECK(res.err == net::io::status_condition::closed);
}

TEST_CASE("reads time out once their deadline passes", "[io][scheduler]")
{
    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    auto start = net::io::clock::now();
    reader.set_read_deadline(start + 50ms);

    std::string received;
    auto        res     = sched.run_to_completion(read_some(&reader, &received));
    auto        elapsed = net::io::clock::now() - start;

    CHECK(res.err == std::errc::timed_out);
    CHECK(elapsed >= 50ms);
    CHECK(elapsed < 2s);
}

TEST_CASE("a deadline earlier than the one the loop is waiting for wakes it up", "[io][scheduler]")
{
    scheduler sched{make_pool()};
    auto [idle_reader, idle_writer]     = make_socket_pair(&sched);
    auto [timed_reader, timed_writer]   = make_socket_pair(&sched);

    // with only this wait, the loop waits without a timeout.
    std::error_condition idle_err;
    std::atomic<bool>    idle_finished = false;
    REQUIRE(sched.spawn(read_until_failure(&idle_reader, &idle_err, &idle_finished)));

    auto timed_read = [](net::socket* sock) -> task<std::pair<net::io::result, net::io::clock::duration>>
    {
        // give the loop time to start waiting.
        std::this_thread::sleep_for(100ms);

        auto start = net::io::clock::now();
        sock->set_read_deadline(start + 50ms);

        std::string received;
        auto        res = co_await read_some(sock, &received);
        co_return std::pair{res, net::io::clock::now() - start};
    };

    auto [res, elapsed] = sched.run_to_completion(timed_read(&timed_reader));

    CHECK(res.err == std::errc::timed_out);
    CHECK(elapsed < 1s);

    // run_to_completion stops the scheduler, which cancels the idle read.
    CHECK(idle_finished.load());
    CHECK(idle_err == std::errc::operation_canceled);
}

TEST_CASE("shutdown cancels pending waits, and lets tasks finish", "[io][scheduler]")
{
    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    std::error_condition err;
    std::atomic<bool>    finished = false;
    REQUIRE(sched.spawn(read_until_failure(&reader, &err, &finished)));

    std::thread run_thread{[&] { sched.run(); }};
    std::this_thread::sleep_for(50ms);

    sched.shutdown();
    run_thread.join();

    CHECK(finished.load());
    CHECK(err == std::errc::operation_canceled);

    CHECK_FALSE(sched.spawn([]() -> task<> { co_return; }()));
}

TEST_CASE("a scheduler that never ran can be destroyed", "[io][scheduler]")
{
    std::error_condition err;
    std::atomic<bool>    finished = false;

    {
        scheduler sched{make_pool()};
        auto [reader, writer] = make_socket_pair(&sched);

        REQUIRE(sched.spawn(read_until_failure(&reader, &err, &finished)));
        std::this_thread::sleep_for(20ms);
    }

    CHECK(finished.load());
    CHECK(err == std::errc::operation_canceled);
}

TEST_CASE("run_to_completion returns the task's result", "[io][scheduler]")
{
    SECTION("value")
    {
        scheduler sched{make_pool()};
        CHECK(sched.run_to_completion([]() -> task<int> { co_return 42; }()) == 42);
    }

    SECTION("exception")
    {
        scheduler sched{make_pool()};
        CHECK_THROWS_AS(sched.run_to_completion(
                            []() -> task<int>
                            {
                                throw std::runtime_error{"boom"};
                                co_return 0;
                            }()),
                        std::runtime_error);
    }
}

TEST_CASE("waits on unregistered handles fail", "[io][scheduler]")
{
    scheduler sched{make_pool()};

    auto wait = [](scheduler* sched) -> task<std::error_condition>
    { co_return co_await sched->wait(1'000'000, net::io::poll_op::read); };

    CHECK(sched.run_to_completion(wait(&sched)) == std::errc::bad_file_descriptor);
}

TEST_CASE("only one task may wait for each operation on a handle", "[io][scheduler]")
{
    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    std::error_condition first_err;
    std::error_condition second_err;
    std::atomic<bool>    first_finished  = false;
    std::atomic<bool>    second_finished = false;

    REQUIRE(sched.spawn(read_until_failure(&reader, &first_err, &first_finished)));
    REQUIRE(sched.spawn(read_until_failure(&reader, &second_err, &second_finished)));

    std::thread run_thread{[&] { sched.run(); }};

    // one fails straight away, while the other waits until shutdown.
    REQUIRE(eventually([&] { return first_finished.load() || second_finished.load(); }));
    auto busy = first_finished.load() ? first_err : second_err;
    CHECK(busy == std::errc::device_or_resource_busy);

    sched.shutdown();
    run_thread.join();
}

TEST_CASE("a read and a write can wait on the same socket at once", "[io][scheduler]")
{
    scheduler sched{make_pool()};
    auto [near, far] = make_socket_pair(&sched);

    // fill the socket's buffer, so the next write has to wait.
    std::size_t      prefilled = 0;
    std::vector<char> chunk(std::size_t{64} * 1'024, 'x');
    while (true)
    {
        auto num = ::send(near.native_handle(), chunk.data(), chunk.size(), 0);
        if (num <= 0) break;
        prefilled += static_cast<std::size_t>(num);
    }

    std::atomic<int> finished = 0;
    net::io::result  read_res;
    net::io::result  write_res;
    std::string      received;

    auto reader = [](net::socket* sock, std::string* received, net::io::result* res, std::atomic<int>* finished) -> task<>
    {
        *res = co_await read_some(sock, received);
        finished->fetch_add(1);
    };

    auto writer = [](net::socket* sock, net::io::result* res, std::atomic<int>* finished) -> task<>
    {
        *res = co_await sock->write(std::as_bytes(std::span{"more"sv}));
        finished->fetch_add(1);
    };

    REQUIRE(sched.spawn(reader(&near, &received, &read_res, &finished)));
    REQUIRE(sched.spawn(writer(&near, &write_res, &finished)));

    std::thread run_thread{[&] { sched.run(); }};

    // both are waiting: drain the other end so the write can finish, and then reply so the read can.
    std::this_thread::sleep_for(50ms);
    CHECK(finished.load() == 0);

    auto drain = [](net::socket* sock, std::size_t expected) -> task<>
    {
        std::vector<std::byte> buf(std::size_t{64} * 1'024);
        std::size_t            total = 0;
        while (total < expected)
        {
            auto res = co_await sock->read(buf);
            if (res.err) throw std::system_error{res.err.value(), res.err.category()};
            total += res.count;
        }

        co_await sock->write(std::as_bytes(std::span{"reply"sv}));
    };
    REQUIRE(sched.spawn(drain(&far, prefilled + 4)));

    REQUIRE(eventually([&] { return finished.load() == 2; }));

    CHECK_FALSE(write_res.err);
    CHECK(write_res.count == 4);
    CHECK_FALSE(read_res.err);
    CHECK(received == "reply");

    sched.shutdown();
    run_thread.join();
}

TEST_CASE("shutting down a listener cancels a pending accept", "[io][scheduler][listener]")
{
    scheduler     sched{make_pool()};
    net::listener listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    std::error_condition accept_err;
    std::atomic<bool>    finished = false;

    auto accept = [](net::listener* listener, std::error_condition* err, std::atomic<bool>* finished) -> task<>
    {
        auto res = co_await listener->accept();
        if (!res.has_value()) *err = res.error();
        finished->store(true);
    };
    REQUIRE(sched.spawn(accept(&listener, &accept_err, &finished)));

    std::thread run_thread{[&] { sched.run(); }};
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(finished.load());

    listener.shutdown();

    REQUIRE(eventually([&] { return finished.load(); }));
    CHECK(accept_err == std::errc::operation_canceled);

    sched.shutdown();
    run_thread.join();
}

TEST_CASE("connections can be accepted, read from, and written to", "[io][scheduler][listener]")
{
    scheduler     sched{make_pool()};
    net::listener listener{&sched, "127.0.0.1", "0", net::network::tcp};
    listener.listen(8);

    auto echo_once = [](net::listener* listener) -> task<>
    {
        auto conn = co_await listener->accept();
        if (!conn.has_value()) co_return;

        std::string received;
        auto        res = co_await read_some(&*conn, &received);
        if (res.err) co_return;

        co_await conn->write(std::as_bytes(std::span{received}));
    };
    REQUIRE(sched.spawn(echo_once(&listener)));

    auto client = [](scheduler* sched, std::string port) -> task<std::string>
    {
        auto connected = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        if (!connected.has_value()) co_return "connect failed: " + connected.error().message();
        auto& conn = *connected;

        auto res = co_await conn.write(std::as_bytes(std::span{"hello"sv}));
        if (res.err) co_return "write failed: " + res.err.message();

        std::string received;
        while (received.size() < 5)
        {
            res = co_await read_some(&conn, &received);
            if (res.err) co_return "read failed: " + res.err.message();
        }

        co_return received;
    };

    CHECK(sched.run_to_completion(client(&sched, local_port(listener))) == "hello");
}

TEST_CASE("run_blocking runs on another thread, and continues on the workers", "[io][scheduler][run_blocking]")
{
    scheduler sched{make_pool()};

    auto check = [](scheduler* sched) -> task<std::pair<bool, bool>>
    {
        auto worker   = std::this_thread::get_id();
        auto blocking = co_await sched->run_blocking([] { return std::this_thread::get_id(); });
        auto after    = std::this_thread::get_id();

        // after may be a different worker, but it's not the blocking thread.
        co_return std::pair{blocking != worker, after != blocking};
    };

    auto [ran_elsewhere, came_back] = sched.run_to_completion(check(&sched));
    CHECK(ran_elsewhere);
    CHECK(came_back);
}

TEST_CASE("run_blocking passes exceptions on", "[io][scheduler][run_blocking]")
{
    scheduler sched{make_pool()};

    auto throws = [](scheduler* sched) -> task<int>
    { co_return co_await sched->run_blocking([]() -> int { throw std::runtime_error{"boom"}; }); };

    CHECK_THROWS_AS(sched.run_to_completion(throws(&sched)), std::runtime_error);
}
