#include "io/scheduler.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
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
#include "io/io.hpp"
#include "io/poll.hpp"
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
    return {
        net::socket{sched, fds[0]},
        net::socket{sched, fds[1]}
    };
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
task<> read_until_failure(net::socket* sock, std::error_code* err, std::atomic<bool>* finished)
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

    CHECK(res.err == net::io::errc::end_of_stream);
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
    auto [idle_reader, idle_writer]   = make_socket_pair(&sched);
    auto [timed_reader, timed_writer] = make_socket_pair(&sched);

    // with only this wait, the loop waits without a timeout.
    std::error_code   idle_err;
    std::atomic<bool> idle_finished = false;
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

TEST_CASE("shutdown from one of the scheduler's tasks throws, rather than waiting for itself", "[io][scheduler]")
{
    scheduler sched{make_pool()};

    const auto shut_down = [](scheduler* sched) -> task<bool>
    {
        try
        {
            sched->shutdown();
        }
        catch (const std::logic_error&)
        {
            co_return true;
        }

        co_return false;
    };

    CHECK(sched.run_to_completion(shut_down(&sched)));
}

TEST_CASE("shutdown cancels pending waits, and lets tasks finish", "[io][scheduler]")
{
    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    std::error_code   err;
    std::atomic<bool> finished = false;
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
    std::error_code   err;
    std::atomic<bool> finished = false;

    // NOTE: a sleep rather than a socket's read, as the socket would be closed first (and the workers poll, whether or
    //       not the scheduler runs).
    const auto sleep = [](scheduler* sched, std::error_code* err, std::atomic<bool>* finished) -> task<>
    {
        *err = co_await sched->sleep_until(net::io::clock::now() + 1h);
        finished->store(true);
    };

    {
        scheduler sched{make_pool()};

        REQUIRE(sched.spawn(sleep(&sched, &err, &finished)));
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

    auto wait = [](scheduler* sched) -> task<std::error_code>
    { co_return co_await sched->wait(1'000'000, net::io::poll_op::read); };

    CHECK(sched.run_to_completion(wait(&sched)) == std::errc::bad_file_descriptor);
}

TEST_CASE("only one task may wait for each operation on a handle", "[io][scheduler]")
{
    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    std::error_code   first_err;
    std::error_code   second_err;
    std::atomic<bool> first_finished  = false;
    std::atomic<bool> second_finished = false;

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
    std::size_t       prefilled = 0;
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

    auto reader =
        [](net::socket* sock, std::string* received, net::io::result* res, std::atomic<int>* finished) -> task<>
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

    std::error_code   accept_err;
    std::atomic<bool> finished = false;

    auto accept = [](net::listener* listener, std::error_code* err, std::atomic<bool>* finished) -> task<>
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
    { co_return co_await sched->run_blocking([] -> int { throw std::runtime_error{"boom"}; }); };

    CHECK_THROWS_AS(sched.run_to_completion(throws(&sched)), std::runtime_error);
}

TEST_CASE("wait_on waits for a notification", "[io][scheduler][wait_on]")
{
    scheduler                  sched{make_pool()};
    std::atomic<std::uint64_t> word{0};
    std::atomic<int>           woken{0};

    auto waiter = [](scheduler* sched, std::atomic<std::uint64_t>* word, std::atomic<int>* woken) -> task<>
    {
        auto err = co_await sched->wait_on(*word, 0);
        if (!err) ++*woken;
    };

    auto notifier = [&](scheduler* sched) -> task<bool>
    {
        // NOTE: no REQUIRE on the worker threads: Catch2's assertions aren't thread-safe.
        for (int i = 0; i < 3; ++i)
            if (!sched->spawn(waiter(sched, &word, &woken))) co_return false;

        // give the waiters time to wait, so it's the notification that wakes them.
        (void)co_await sched->sleep_until(net::io::clock::now() + 50ms);
        bool none_early = woken == 0;

        ++word;
        sched->notify_all(word);
        co_return none_early;
    };

    CHECK(sched.run_to_completion(notifier(&sched)));
    CHECK(woken == 3);
}

TEST_CASE("wait_on doesn't wait once the word has changed", "[io][scheduler][wait_on]")
{
    scheduler                  sched{make_pool()};
    std::atomic<std::uint64_t> word{1};

    // with no notification to come, it would wait forever.
    auto wait = [](scheduler* sched, std::atomic<std::uint64_t>* word) -> task<std::error_code>
    { co_return co_await sched->wait_on(*word, 0); };

    CHECK_FALSE(sched.run_to_completion(wait(&sched, &word)));
}

TEST_CASE("notify_one wakes waiters in order", "[io][scheduler][wait_on]")
{
    scheduler                  sched{make_pool()};
    std::atomic<std::uint64_t> word{0};
    std::mutex                 order_mu;
    std::vector<int>           order;

    auto waiter = [&](scheduler* sched, int id) -> task<>
    {
        auto err = co_await sched->wait_on(word, 0);
        if (err) co_return;

        std::scoped_lock lock{order_mu};
        order.push_back(id);
    };

    auto notifier = [&](scheduler* sched) -> task<>
    {
        // one at a time, so they start waiting in order.
        for (int id = 0; id < 3; ++id)
        {
            if (!sched->spawn(waiter(sched, id))) co_return;
            (void)co_await sched->sleep_until(net::io::clock::now() + 20ms);
        }

        word = 1;
        for (int i = 0; i < 3; ++i)
        {
            sched->notify_one(word);
            (void)co_await sched->sleep_until(net::io::clock::now() + 20ms);
        }
    };

    sched.run_to_completion(notifier(&sched));
    CHECK(order == std::vector{0, 1, 2});
}

TEST_CASE("wait_on times out", "[io][scheduler][wait_on]")
{
    scheduler                  sched{make_pool()};
    std::atomic<std::uint64_t> word{0};

    auto wait = [](scheduler* sched, std::atomic<std::uint64_t>* word) -> task<std::error_code>
    { co_return co_await sched->wait_on(*word, 0, net::io::clock::now() + 20ms); };

    CHECK(sched.run_to_completion(wait(&sched, &word)) == std::errc::timed_out);
}

TEST_CASE("sleep_until sleeps, and stopping cancels sleeps and waits", "[io][scheduler][sleep_until]")
{
    SECTION("sleeping")
    {
        scheduler sched{make_pool()};

        auto sleep = [](scheduler* sched) -> task<std::pair<std::error_code, net::io::clock::duration>>
        {
            auto start = net::io::clock::now();
            auto err   = co_await sched->sleep_until(start + 50ms);
            co_return std::pair{err, net::io::clock::now() - start};
        };

        auto [err, slept] = sched.run_to_completion(sleep(&sched));
        CHECK_FALSE(err);
        CHECK(slept >= 50ms);
    }

    SECTION("stopping")
    {
        scheduler                  sched{make_pool()};
        std::atomic<std::uint64_t> word{0};
        std::error_code            sleep_err;
        std::error_code            wait_err;

        auto sleeper = [](scheduler* sched, std::error_code* err) -> task<>
        { *err = co_await sched->sleep_until(net::io::clock::now() + 1h); };

        auto waiter = [](scheduler* sched, std::atomic<std::uint64_t>* word, std::error_code* err) -> task<>
        { *err = co_await sched->wait_on(*word, 0); };

        auto stopper = [&](scheduler* sched) -> task<>
        {
            if (!sched->spawn(sleeper(sched, &sleep_err)) || !sched->spawn(waiter(sched, &word, &wait_err))) co_return;
            (void)co_await sched->sleep_until(net::io::clock::now() + 20ms);
        };

        // run_to_completion stops the scheduler once the stopper finishes, which must cancel the others.
        auto start = net::io::clock::now();
        sched.run_to_completion(stopper(&sched));

        CHECK(net::io::clock::now() - start < 10s);
        CHECK(sleep_err == std::errc::operation_canceled);
        CHECK(wait_err == std::errc::operation_canceled);
    }
}

TEST_CASE("socket timeouts limit each wait, rather than the whole read", "[io][socket][timeout]")
{
    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);
    reader.set_read_timeout(150ms);

    // a byte every 30ms, for longer than the timeout altogether (with room for a slow machine to be late).
    auto trickle = [](scheduler* sched, net::socket* writer) -> task<>
    {
        for (int i = 0; i < 8; ++i)
        {
            (void)co_await sched->sleep_until(net::io::clock::now() + 30ms);
            co_await writer->write(std::as_bytes(std::span{"x"sv}));
        }
    };

    auto read_all = [](net::socket* reader) -> task<std::pair<std::string, std::error_code>>
    {
        std::string got;
        while (true)
        {
            auto res = co_await read_some(reader, &got);
            if (res.err) co_return std::pair{got, res.err};
        }
    };

    REQUIRE(sched.spawn(trickle(&sched, &writer)));
    auto [got, err] = sched.run_to_completion(read_all(&reader));
    CHECK(got == "xxxxxxxx");
    CHECK(err == std::errc::timed_out);
}

TEST_CASE("socket write timeouts", "[io][socket][timeout]")
{
    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);
    writer.set_write_timeout(100ms);

    // nothing reads, so the socket's buffers fill, and the write has to wait.
    auto write_lots = [](net::socket* writer) -> task<std::pair<std::size_t, std::error_code>>
    {
        std::vector<std::byte> lots(static_cast<std::size_t>(64 * 1'024) * 1'024);
        auto                   res = co_await writer->write(lots);
        co_return std::pair{res.count, res.err};
    };

    auto start        = net::io::clock::now();
    auto [count, err] = sched.run_to_completion(write_lots(&writer));
    auto elapsed      = net::io::clock::now() - start;

    CHECK(err == std::errc::timed_out);
    CHECK(count > 0);
    CHECK(elapsed >= 100ms);
    CHECK(elapsed < test_timeout);
}

TEST_CASE("a scheduler whose workers poll for another scheduler polls in run()", "[io][scheduler]")
{
    auto      pool = make_pool();
    scheduler first{pool};
    scheduler second{pool};

    const auto sleep = [](scheduler* sched) -> net::coro::task<std::error_code>
    { co_return co_await sched->sleep_until(net::io::clock::now() + 10ms); };

    CHECK_FALSE(second.run_to_completion(sleep(&second)));
    CHECK_FALSE(first.run_to_completion(sleep(&first)));
}

TEST_CASE("socket write timeouts limit each wait, rather than the whole write", "[io][socket][timeout]")
{
    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);
    // NOTE: the timeout leaves plenty of room for a busy machine (e.g. CI's) to pause, as each wait is ~10ms (below).
    constexpr auto timeout = 500ms;
    writer.set_write_timeout(timeout);

    constexpr std::size_t total = std::size_t{2} * 1'024 * 1'024;

    // more than the socket's buffers hold, so the write has to wait for the reader several times.
    auto write_all_of_it = [](net::socket* writer) -> task<std::pair<std::size_t, std::error_code>>
    {
        std::vector<std::byte> lots(total);
        auto                   res = co_await writer->write(lots);
        co_return std::pair{res.count, res.err};
    };

    // reads a little at a time (16 KiB every 10ms, so over a second for all of it): the write makes progress well
    // within the timeout, but takes longer altogether. It stops if the write fails (e.g. times out), as the rest won't
    // come; otherwise, it reads it all, as the write finishes once its data is in the socket's buffers, before it's
    // read.
    std::size_t       read         = 0;
    std::atomic<bool> write_failed = false;
    std::thread       slow_reader{[&, fd = reader.native_handle()]
                                  {
                                std::vector<char> buf(std::size_t{16} * 1'024);
                                while (read < total && !write_failed.load())
                                {
                                    std::this_thread::sleep_for(10ms);

                                    // NOTE: the socket's non-blocking, so this may find nothing yet.
                                    auto got = ::recv(fd, buf.data(), buf.size(), 0);
                                    if (got > 0) read += static_cast<std::size_t>(got);
                                    else if (got == 0) break;
                                }
                                  }};

    auto start        = net::io::clock::now();
    auto [count, err] = sched.run_to_completion(write_all_of_it(&writer));
    auto elapsed      = net::io::clock::now() - start;

    if (err) write_failed = true;
    slow_reader.join();

    CHECK_FALSE(err);
    CHECK(count == total);
    CHECK(read == total);

    // or the test shows nothing: the timeout would have ended a write limited altogether.
    CHECK(elapsed > timeout);
}

TEST_CASE("a coroutine destroyed while it waits to read a socket is neither resumed, nor holds it", "[io][socket]")
{
    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    {
        std::string ignored;
        auto        waiting = read_some(&reader, &ignored);

        // nothing to read yet, so it waits.
        waiting.get_handle().resume();
        REQUIRE_FALSE(waiting.is_ready());
    }

    // had its wait been left, the scheduler would now read into the destroyed coroutine's buffer, and resume it (which
    // ASan reports), and the next read would find the socket busy.
    REQUIRE(::send(writer.native_handle(), "x", 1, 0) == 1);

    std::string received;
    auto        res = sched.run_to_completion(read_some(&reader, &received));

    CHECK_FALSE(res.err);
    CHECK(received == "x");
}

namespace
{

enum class wait_kind : std::uint8_t
{
    sleep,
    wait_on,
    handle,
};

// stoppable_wait waits (as kind says) for an hour, for a notification about word, or for fd to be readable, given stop.
task<std::error_code> stoppable_wait(
    scheduler* sched, wait_kind kind, std::stop_token stop, net::io::handle fd, std::atomic<std::uint64_t>* word)
{
    switch (kind)
    {
    case wait_kind::sleep: co_return co_await sched->sleep_until(net::io::clock::now() + 1h, std::move(stop));
    case wait_kind::wait_on: co_return co_await sched->wait_on(*word, 0, std::nullopt, std::move(stop));
    case wait_kind::handle: co_return co_await sched->wait(fd, net::io::poll_op::read, std::move(stop));
    }

    co_return std::make_error_code(std::errc::invalid_argument);
}

// record_wait waits as stoppable_wait does, and records how it ended, counting each time it's resumed.
task<> record_wait(scheduler*                  sched,
                   wait_kind                   kind,
                   std::stop_token             stop,
                   net::io::handle             fd,
                   std::atomic<std::uint64_t>* word,
                   std::error_code*            err,
                   std::atomic<int>*           finished)
{
    *err = co_await stoppable_wait(sched, kind, std::move(stop), fd, word);
    finished->fetch_add(1);
}

}

TEST_CASE("requesting stop ends the wait given its token", "[io][scheduler][stop]")
{
    const auto kind = GENERATE(wait_kind::sleep, wait_kind::wait_on, wait_kind::handle);
    CAPTURE(kind);

    std::stop_source           source;
    std::atomic<std::uint64_t> word{0};
    std::error_code            err;
    std::atomic<int>           finished{0};

    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    REQUIRE(sched.spawn(record_wait(&sched, kind, source.get_token(), reader.native_handle(), &word, &err, &finished)));

    // it waits until stop is requested.
    std::this_thread::sleep_for(50ms);
    CHECK(finished.load() == 0);

    source.request_stop();
    REQUIRE(eventually([&] { return finished.load() == 1; }));
    CHECK(err == std::errc::operation_canceled);
}

TEST_CASE("a wait whose token was stopped already fails at once", "[io][scheduler][stop]")
{
    const auto kind = GENERATE(wait_kind::sleep, wait_kind::wait_on, wait_kind::handle);
    CAPTURE(kind);

    std::stop_source           source;
    std::atomic<std::uint64_t> word{0};
    source.request_stop();

    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    // even with something to read, or a notification that came already: the token comes first.
    REQUIRE(::send(writer.native_handle(), "x", 1, 0) == 1);
    if (kind == wait_kind::wait_on) word = 1;

    auto start = net::io::clock::now();
    auto err = sched.run_to_completion(stoppable_wait(&sched, kind, source.get_token(), reader.native_handle(), &word));

    CHECK(err == std::errc::operation_canceled);
    CHECK(net::io::clock::now() - start < test_timeout);
}

TEST_CASE("a token that can't be stopped is as none", "[io][scheduler][stop]")
{
    scheduler sched{make_pool()};

    std::optional<std::stop_source> gone{std::in_place};
    auto                            orphaned = gone->get_token();
    gone.reset();
    REQUIRE_FALSE(orphaned.stop_possible());

    const auto token = GENERATE_COPY(std::stop_token{}, orphaned);

    const auto sleep = [](scheduler*      sched,
                          std::stop_token stop) -> task<std::pair<std::error_code, net::io::clock::duration>>
    {
        auto start = net::io::clock::now();
        auto err   = co_await sched->sleep_until(start + 20ms, std::move(stop));
        co_return std::pair{err, net::io::clock::now() - start};
    };

    auto [err, slept] = sched.run_to_completion(sleep(&sched, token));
    CHECK_FALSE(err);
    CHECK(slept >= 20ms);
}

TEST_CASE("requesting stop ends only the waits given its token", "[io][scheduler][stop]")
{
    std::stop_source           stopped;
    std::stop_source           other;
    std::atomic<std::uint64_t> word{0};
    std::error_code            stopped_err;
    std::error_code            other_err;
    std::error_code            none_err;
    std::atomic<int>           stopped_finished{0};
    std::atomic<int>           others_finished{0};

    scheduler sched{make_pool()};

    REQUIRE(sched.spawn(
        record_wait(&sched, wait_kind::wait_on, stopped.get_token(), -1, &word, &stopped_err, &stopped_finished)));
    REQUIRE(sched.spawn(
        record_wait(&sched, wait_kind::wait_on, other.get_token(), -1, &word, &other_err, &others_finished)));
    REQUIRE(sched.spawn(record_wait(&sched, wait_kind::wait_on, {}, -1, &word, &none_err, &others_finished)));
    std::this_thread::sleep_for(50ms);

    stopped.request_stop();
    REQUIRE(eventually([&] { return stopped_finished.load() == 1; }));
    CHECK(stopped_err == std::errc::operation_canceled);

    // the others wait on, until they're notified.
    std::this_thread::sleep_for(50ms);
    CHECK(others_finished.load() == 0);

    word = 1;
    sched.notify_all(word);
    REQUIRE(eventually([&] { return others_finished.load() == 2; }));
    CHECK_FALSE(other_err);
    CHECK_FALSE(none_err);
}

namespace
{

// race_after waits for started, and then a little longer (a different time each round, so stop comes before a wait
// begins, while it waits, and after it has ended), and runs func.
template<typename F>
void race_after(const std::atomic<bool>& started, int round, F func)
{
    while (!started.load())
    {}

    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds{round % 64};
    while (std::chrono::steady_clock::now() < until)
    {}

    func();
}

// racing_wait waits as stoppable_wait does (but its sleep is about to end), and records how it ended, counting each
// time it's resumed.
task<> racing_wait(scheduler*                  sched,
                   wait_kind                   kind,
                   std::stop_token             stop,
                   net::io::handle             fd,
                   std::atomic<std::uint64_t>* word,
                   std::atomic<bool>*          started,
                   std::error_code*            err,
                   std::atomic<int>*           finished)
{
    started->store(true);
    if (kind == wait_kind::sleep) *err = co_await sched->sleep_until(net::io::clock::now() + 30us, std::move(stop));
    else *err = co_await stoppable_wait(sched, kind, std::move(stop), fd, word);
    finished->fetch_add(1);
}

// complete ends a racing_wait as kind would besides stop: with a notification, or something to read. (A sleep ends by
// itself.)
void complete(wait_kind kind, scheduler& sched, std::atomic<std::uint64_t>& word, const net::socket& writer)
{
    if (kind == wait_kind::wait_on)
    {
        word = 1;
        sched.notify_all(word);
    }
    else if (kind == wait_kind::handle)
    {
        (void)::send(writer.native_handle(), "x", 1, 0);
    }
}

}

TEST_CASE("a wait racing stop ends once, either way", "[io][scheduler][stop]")
{
    const auto kind = GENERATE(wait_kind::sleep, wait_kind::wait_on, wait_kind::handle);
    CAPTURE(kind);

    constexpr int rounds = 300;

    std::atomic<std::uint64_t> word{0};
    std::error_code            err;
    std::atomic<int>           finished{0};
    std::atomic<bool>          started = false;

    scheduler sched{make_pool()};

    for (int round = 0; round < rounds; ++round)
    {
        std::stop_source source;
        word    = 0;
        started = false;

        // NOTE: new sockets each round, as a byte that arrives after a wait was canceled leaves the handle ready, so
        //       the next wait wouldn't wait.
        auto [reader, writer] = make_socket_pair(&sched);

        REQUIRE(sched.spawn(
            racing_wait(&sched, kind, source.get_token(), reader.native_handle(), &word, &started, &err, &finished)));

        std::thread completer{[&] { race_after(started, round * 7, [&] { complete(kind, sched, word, writer); }); }};
        race_after(started, round, [&] { source.request_stop(); });
        completer.join();

        REQUIRE(eventually([&] { return finished.load() == round + 1; }));
        CAPTURE(round, err.message());
        REQUIRE((!err || err == std::errc::operation_canceled));
    }

    // a wait resumed twice would have counted twice (or crashed).
    std::this_thread::sleep_for(20ms);
    CHECK(finished.load() == rounds);
}

TEST_CASE("a socket's read canceled by its stop token reads nothing, so the next read gets the data",
          "[io][socket][stop]")
{
    std::stop_source source;
    scheduler        sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    const auto check =
        [](scheduler* sched, net::socket* reader, net::socket* writer, std::stop_source* source) -> task<std::string>
    {
        reader->set_stop_token(source->get_token());

        // stop is requested while the read waits.
        std::thread stopper{[source]
                            {
                                std::this_thread::sleep_for(50ms);
                                source->request_stop();
                            }};

        std::string got;
        auto        res = co_await read_some(reader, &got);
        stopper.join();
        if (res.err != std::errc::operation_canceled) co_return "the first read: " + res.err.message();
        if (res.count != 0 || !got.empty()) co_return "the first read read something";

        // stop requested already fails a read that has to wait, at once.
        res = co_await read_some(reader, &got);
        if (res.err != std::errc::operation_canceled) co_return "the second read: " + res.err.message();

        // but a read that needn't wait isn't affected.
        co_await writer->write(std::as_bytes(std::span{"hello"sv}));
        res = co_await read_some(reader, &got);
        if (res.err) co_return "the third read: " + res.err.message();

        // and without the token, a read waits for what comes.
        reader->set_stop_token({});
        const auto write_later = [](scheduler* sched, net::socket* writer) -> task<>
        {
            (void)co_await sched->sleep_until(net::io::clock::now() + 20ms);
            co_await writer->write(std::as_bytes(std::span{" world"sv}));
        };
        if (!sched->spawn(write_later(sched, writer))) co_return "spawn failed";

        res = co_await read_some(reader, &got);
        if (res.err) co_return "the fourth read: " + res.err.message();
        co_return got;
    };

    CHECK(sched.run_to_completion(check(&sched, &reader, &writer, &source)) == "hello world");
}

TEST_CASE("a socket's read racing stop loses nothing", "[io][socket][stop]")
{
    constexpr int rounds = 300;

    std::string       got;
    std::error_code   err;
    std::atomic<int>  finished{0};
    std::atomic<bool> started = false;

    scheduler sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);

    // read_one reads, and if stop canceled it, reads again without the token: either way, it gets the byte.
    const auto read_one = [](net::socket*       reader,
                             std::string*       got,
                             std::atomic<bool>* started,
                             std::error_code*   err,
                             std::atomic<int>*  finished) -> task<>
    {
        started->store(true);
        auto res = co_await read_some(reader, got);
        if (res.err == std::errc::operation_canceled)
        {
            reader->set_stop_token({});
            res = co_await read_some(reader, got);
        }

        *err = res.err;
        finished->fetch_add(1);
    };

    for (int round = 0; round < rounds; ++round)
    {
        std::stop_source source;
        reader.set_stop_token(source.get_token());
        started = false;

        REQUIRE(sched.spawn(read_one(&reader, &got, &started, &err, &finished)));

        std::thread sender{
            [&] { race_after(started, round * 7, [&] { (void)::send(writer.native_handle(), "x", 1, 0); }); }};
        race_after(started, round, [&] { source.request_stop(); });
        sender.join();

        REQUIRE(eventually([&] { return finished.load() == round + 1; }));
        CAPTURE(round, err.message());
        REQUIRE_FALSE(err);
    }

    CHECK(got == std::string(rounds, 'x'));
}

TEST_CASE("a socket's write canceled by its stop token reports what it wrote", "[io][socket][stop]")
{
    std::stop_source source;
    scheduler        sched{make_pool()};
    auto [reader, writer] = make_socket_pair(&sched);
    writer.set_stop_token(source.get_token());

    // more than the socket's buffers hold, read slowly: the write makes some progress, and then it's stopped.
    std::atomic<std::size_t> read = 0;
    std::thread              slow_reader{[&, fd = reader.native_handle()]
                                         {
                                std::vector<char> buf(std::size_t{16} * 1'024);
                                while (true)
                                {
                                    // slowly until the write's stopped, then to the end.
                                    if (!source.stop_requested()) std::this_thread::sleep_for(5ms);

                                    // NOTE: the socket's non-blocking, so this may find nothing yet.
                                    auto got = ::recv(fd, buf.data(), buf.size(), 0);
                                    if (got > 0) read += static_cast<std::size_t>(got);
                                    else if (got == 0) break;
                                    else std::this_thread::sleep_for(1ms);
                                }
                                         }};

    const auto write_lots = [](net::socket* writer) -> task<net::io::result>
    {
        std::vector<std::byte> lots(std::size_t{64} * 1'024 * 1'024);
        co_return co_await writer->write(lots);
    };

    std::thread stopper{[&]
                        {
                            std::this_thread::sleep_for(100ms);
                            source.request_stop();
                        }};

    auto res = sched.run_to_completion(write_lots(&writer));
    stopper.join();

    // the reader then reads what was written, to the end.
    writer.shutdown_write();
    slow_reader.join();

    CHECK(res.err == std::errc::operation_canceled);
    CHECK(res.count > 0);
    CHECK(res.count == read.load());
}

TEST_CASE("run_blocking given a stop token", "[io][scheduler][run_blocking][stop]")
{
    scheduler sched{make_pool()};

    SECTION("doesn't run func if stop was requested")
    {
        std::stop_source source;
        source.request_stop();

        const auto run = [](scheduler* sched, std::stop_token stop) -> task<std::pair<bool, std::error_code>>
        {
            bool ran = false;
            auto res = co_await sched->run_blocking([&ran] { ran = true; }, std::move(stop));
            co_return std::pair{ran, res.has_value() ? std::error_code{} : res.error()};
        };

        auto [ran, err] = sched.run_to_completion(run(&sched, source.get_token()));
        CHECK_FALSE(ran);
        CHECK(err == std::errc::operation_canceled);
    }

    SECTION("returns func's result")
    {
        const auto run = [](scheduler* sched) -> task<int>
        {
            std::stop_source source;
            auto             res = co_await sched->run_blocking([] { return 42; }, source.get_token());
            co_return res.value_or(0);
        };

        CHECK(sched.run_to_completion(run(&sched)) == 42);
    }

    SECTION("passes the token to a func that takes one")
    {
        std::stop_source source;

        // it runs until stop is requested, which it only sees through the token.
        const auto run = [](scheduler* sched, std::stop_token stop) -> task<bool>
        {
            auto res = co_await sched->run_blocking(
                [](const std::stop_token& stop)
                {
                    const auto give_up_at = std::chrono::steady_clock::now() + test_timeout;
                    while (!stop.stop_requested())
                    {
                        if (std::chrono::steady_clock::now() > give_up_at) return false;
                        std::this_thread::sleep_for(1ms);
                    }
                    return true;
                },
                std::move(stop));
            co_return res.value_or(false);
        };

        std::thread stopper{[&]
                            {
                                std::this_thread::sleep_for(50ms);
                                source.request_stop();
                            }};
        auto        saw_stop = sched.run_to_completion(run(&sched, source.get_token()));
        stopper.join();

        CHECK(saw_stop);
    }
}

TEST_CASE("the scheduler's stop token is stopped by request_stop", "[io][scheduler][stop]")
{
    std::atomic<bool> called = false;

    scheduler sched{make_pool()};
    auto      token = sched.stop_token();
    REQUIRE(token.stop_possible());
    REQUIRE_FALSE(token.stop_requested());

    std::stop_callback on_stop{token, [&] { called = true; }};

    sched.request_stop();
    CHECK(token.stop_requested());
    CHECK(called.load());
}
