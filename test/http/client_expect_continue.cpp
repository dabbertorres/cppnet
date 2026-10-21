#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/connection_pool.hpp"
#include "http/h2_client.hpp"
#include "http/headers.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "url.hpp"
#include "util/defer.hpp"

// The client's Expect: 100-continue (client_options::expect_continue_timeout): over HTTP/1.1, the HTTP/2 plan's stage
// 6, fifth part; over HTTP/2, its sixth (at the end).
namespace
{

namespace http = net::http;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// order is what happened to a request, in order: "100" (or another status) for each interim response on_informational
// was called with, and "read" for each read of its body. Both happen on the task sending the request, but a server's
// task may look, so it's locked.
struct order
{
    void note(std::string what)
    {
        const std::scoped_lock lock{mu};
        events.push_back(std::move(what));
    }

    [[nodiscard]] std::vector<std::string> seen() const
    {
        const std::scoped_lock lock{mu};
        return events;
    }

    [[nodiscard]] std::size_t reads() const
    {
        const std::scoped_lock lock{mu};
        return static_cast<std::size_t>(std::ranges::count(events, std::string{"read"}));
    }

    // callback is an on_informational that notes each interim response's status.
    std::function<void(http::status, const http::headers&)> callback()
    {
        return [this](http::status code, const http::headers& /*fields*/)
        { note(std::to_string(static_cast<int>(code))); };
    }

    mutable std::mutex       mu;
    std::vector<std::string> events;
};

// noting_reader is a request's body, which notes each of its reads in an order.
class noting_reader final : public net::io::reader
{
public:
    noting_reader(std::string content, order* log)
        : content{std::move(content)}
        , log{log}
    {}

    task<net::io::result> read(std::span<std::byte> data) override
    {
        log->note("read");

        const auto count = std::min(data.size(), content.size() - offset);
        std::ranges::copy(std::as_bytes(std::span{content}).subspan(offset, count), data.begin());
        offset += count;

        if (count == 0 && !data.empty()) co_return {.err = make_error_code(net::io::errc::end_of_stream)};
        co_return {.count = count};
    }

    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

private:
    std::string content;
    std::size_t offset = 0;
    order*      log;
};

// post is a request to url with Expect: 100-continue and content as its body (with its Content-Length), whose reads,
// and interim responses, are noted in log.
http::client_request post(const std::string& url, std::string_view content, order* log)
{
    http::client_request req{.method = http::request_method::POST, .uri = net::url::parse(url).value()};
    req.headers.set("Expect", "100-continue");
    req.headers.set_content_length(content.size());
    req.body             = std::make_unique<noting_reader>(std::string{content}, log);
    req.on_informational = log->callback();
    return req;
}

http::client_options options_with(std::chrono::milliseconds expect_continue_timeout)
{
    http::client_options options;
    options.expect_continue_timeout = expect_continue_timeout;
    return options;
}

// outcome is what a request came to: its response's status and body, or the error.
struct outcome
{
    int             status = 0;
    std::string     body;
    std::error_code err;
};

task<outcome> send(http::client* client, http::client_request req)
{
    outcome out;

    auto resp = co_await client->send(std::move(req));
    if (!resp.has_value())
    {
        out.err = resp.error();
        co_return out;
    }

    out.status = static_cast<int>(resp->status_code);

    auto body = co_await net::io::read_all(*resp->body);
    if (body.has_value()) out.body = std::move(*body);
    else out.err = body.error();
    co_return out;
}

// served is cppnet's server on 127.0.0.1, which sends 100 (Continue) when a handler first reads the body.
struct served
{
    explicit served(net::io::scheduler* sched, const http::server_config& config = {})
        : server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), routes(), config}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    [[nodiscard]] std::string url(std::string_view path) const
    {
        return std::format("http://127.0.0.1:{}{}", server.local_port(), path);
    }

    static http::router routes()
    {
        http::router r;

        // the request's body, read, and sent back.
        r.POST("/echo",
               [](http::server_request& req, http::response_writer& resp) -> task<void>
               {
                   auto body = co_await net::io::read_all(*req.body);
                   if (!body.has_value()) throw std::system_error{body.error()};

                   auto* out = co_await resp.send(http::status::OK, body->size());
                   co_await net::io::write_all(*out, *body);
               });

        // 413, without reading the body.
        r.POST("/refuse",
               [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
               { (void)co_await resp.send(http::status::PAYLOAD_TOO_LARGE, 0); });

        return r;
    }

    http::server server;
};

}

TEST_CASE("an HTTP/1.1 request expecting 100 sends its body once the server's 100 comes", "[http][client][expect]")
{
    net::io::scheduler sched{make_pool()};
    served             server{&sched};

    // long enough that the body can only go for the 100.
    http::client client{&sched, options_with(30s)};

    order      log;
    const auto out = sched.run_to_completion(send(&client, post(server.url("/echo"), "hello", &log)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.body == "hello");

    // the 100 (once), then the body's reads.
    const auto seen = log.seen();
    CAPTURE(seen);
    REQUIRE(seen.size() >= 2);
    CHECK(seen.front() == "100");
    CHECK(std::ranges::count(seen, std::string{"100"}) == 1);
    CHECK(seen[1] == "read");

    // and the connection's kept.
    CHECK(client.stats() == http::connection_stats{.open = 1, .idle = 1});
}

TEST_CASE("an HTTP/1.1 request expecting 100 doesn't send its body if the server answers first, and closes",
          "[http][client][expect]")
{
    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_with(30s)};

    order      log;
    const auto out = sched.run_to_completion(send(&client, post(server.url("/refuse"), "hello", &log)));

    CAPTURE(out.err.message());
    CHECK(out.status == 413);
    CHECK(out.body.empty());

    // cppnet's server says Connection: close, as it never read the body: so it isn't sent, nor the connection reused.
    CHECK(log.reads() == 0);
    CHECK(log.seen().empty());
    CHECK(client.stats() == http::connection_stats{});
}

namespace
{

// raw_server is a server the tests play, from the raw bytes, on 127.0.0.1: each test's script accepts its connections
// and answers their requests, noting what it saw for the test to check once it's finished.
struct raw_server
{
    explicit raw_server(net::io::scheduler* sched)
        : sched{sched}
        , listener{net::listener::make(sched, "127.0.0.1", "0").value()}
    {}

    [[nodiscard]] std::string url() const { return std::format("http://127.0.0.1:{}/", listener.local_port()); }

    // run runs a script on the scheduler, noting when it's finished.
    void run(task<> (*script)(raw_server*))
    {
        const auto wrapped = [](raw_server* server, task<> (*script)(raw_server*)) -> task<>
        {
            const net::util::defer done{[server] noexcept { test::stop_probe::bump(server->sched, server->finished); }};
            co_await script(server);
        };
        REQUIRE(sched->spawn(wrapped(this, script)));
    }

    // done waits (up to 5 seconds) for the script to finish, returning whether it did.
    [[nodiscard]] task<bool> done() const { return test::stop_probe::reach(sched, finished, 1); }

    // accept accepts a connection, counting it.
    task<std::optional<net::tcp_socket>> accept()
    {
        auto conn = co_await listener.accept();
        if (!conn.has_value()) co_return std::nullopt;
        ++accepted;
        co_return std::move(*conn);
    }

    // read_head reads a request's head (without the empty line that ends it), noting it; what's read past it is left in
    // pending. Nothing, if the connection ends first.
    task<std::optional<std::string>> read_head(net::tcp_socket& conn, std::string& pending)
    {
        std::array<std::byte, 1'024> buf{};
        while (!pending.contains("\r\n\r\n"))
        {
            auto res = co_await conn.read(buf);
            pending.append(reinterpret_cast<const char*>(buf.data()), res.count);
            if (res.err) co_return std::nullopt;
        }

        const auto end  = pending.find("\r\n\r\n");
        auto       head = pending.substr(0, end);
        pending.erase(0, end + 4);
        heads.push_back(head);
        co_return head;
    }

    // read_body reads length bytes of a body (some may be in pending), noting it, or what came of it before the
    // connection ended (or the deadline passed, if there's one).
    task<std::string> read_body(net::tcp_socket&                          conn,
                                std::string&                              pending,
                                std::size_t                               length,
                                std::optional<net::io::clock::time_point> deadline = std::nullopt)
    {
        conn.set_read_deadline(deadline);

        std::array<std::byte, 1'024> buf{};
        while (pending.size() < length)
        {
            auto res = co_await conn.read(buf);
            pending.append(reinterpret_cast<const char*>(buf.data()), res.count);
            if (res.err) break;
        }

        conn.set_read_deadline(std::nullopt);
        auto body = pending.substr(0, std::min(length, pending.size()));
        pending.erase(0, body.size());
        bodies.push_back(body);
        co_return body;
    }

    // rest reads until the client closes the connection, returning what came (with pending's).
    static task<std::string> rest(net::tcp_socket& conn, std::string pending)
    {
        std::array<std::byte, 1'024> buf{};
        while (true)
        {
            auto res = co_await conn.read(buf);
            pending.append(reinterpret_cast<const char*>(buf.data()), res.count);
            if (res.err) co_return pending;
        }
    }

    // nothing_for is whether nothing comes on the connection (nor is in pending) for a while: e.g. the body of a
    // request that should be waiting for 100 (Continue).
    static task<bool> nothing_for(net::tcp_socket& conn, const std::string& pending, net::io::clock::duration wait)
    {
        if (!pending.empty()) co_return false;

        conn.set_read_deadline(net::io::clock::now() + wait);
        std::array<std::byte, 1'024> buf{};
        const auto                   res = co_await conn.read(buf);
        conn.set_read_deadline(std::nullopt);
        co_return res.count == 0 && res.err == std::errc::timed_out;
    }

    static task<bool> send(net::tcp_socket& conn, std::string_view bytes)
    {
        co_return !(co_await net::io::write_all(conn, bytes)).err;
    }

    net::io::scheduler* sched;
    net::listener       listener;

    // what the script saw, read once it's finished.
    std::vector<std::string> heads;
    std::vector<std::string> bodies;
    std::string              after; // what came after a response that the client mustn't answer with a body
    bool                     quiet = false;

    std::atomic<int>           accepted{0};
    std::atomic<std::uint64_t> finished{0};
};

// echo answers a request whose body is to come (5 bytes) with 200 and the body.
task<> echo(raw_server* server, net::tcp_socket& conn, std::string& pending)
{
    auto body = co_await server->read_body(conn, pending, 5);
    (void)co_await raw_server::send(conn,
                                    std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n{}", body.size(), body));
}

// expectation_failed answers the first request (on its first connection) with 417, and the second (on another) by
// echoing its body.
template<bool close>
task<> expectation_failed(raw_server* server)
{
    {
        auto conn = co_await server->accept();
        if (!conn.has_value()) co_return;

        std::string pending;
        if (!co_await server->read_head(*conn, pending)) co_return;
        if (!co_await raw_server::send(*conn,
                                       close ? "HTTP/1.1 417 Expectation Failed\r\nConnection: close\r\n"
                                               "Content-Length: 0\r\n\r\n"sv
                                             : "HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n"sv))
            co_return;

        // the client closes the connection without sending the body, either way.
        server->after = co_await raw_server::rest(*conn, std::move(pending));
    }

    auto conn = co_await server->accept();
    if (!conn.has_value()) co_return;

    std::string pending;
    if (!co_await server->read_head(*conn, pending)) co_return;
    co_await echo(server, *conn, pending);
}

// silent never answers until it has the body (but with 103 (Early Hints) first, if hint), then echoes it.
template<bool hint>
task<> silent(raw_server* server)
{
    auto conn = co_await server->accept();
    if (!conn.has_value()) co_return;

    std::string pending;
    if (!co_await server->read_head(*conn, pending)) co_return;
    if (hint && !co_await raw_server::send(*conn, "HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n"sv))
        co_return;

    // NOTE: a deadline of its own, so a client that went on waiting (e.g. for response_header_timeout's 30 seconds)
    //       fails the test sooner.
    auto body = co_await server->read_body(*conn, pending, 5, net::io::clock::now() + 5s);
    (void)co_await raw_server::send(*conn,
                                    std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n{}", body.size(), body));
}

// hints_then_continue sends 103 (Early Hints), then (once nothing's come for a while: the client still waits) 100
// (Continue), then echoes the body.
task<> hints_then_continue(raw_server* server)
{
    auto conn = co_await server->accept();
    if (!conn.has_value()) co_return;

    std::string pending;
    if (!co_await server->read_head(*conn, pending)) co_return;
    if (!co_await raw_server::send(*conn, "HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n"sv)) co_return;

    server->quiet = co_await raw_server::nothing_for(*conn, pending, 200ms);
    if (!co_await raw_server::send(*conn, "HTTP/1.1 100 Continue\r\n\r\n"sv)) co_return;
    co_await echo(server, *conn, pending);
}

// answer_first answers with 200 before the body, keeping the connection (and reading the body, then the next request,
// which it answers too), or closing it (noting what came after).
template<bool close>
task<> answer_first(raw_server* server)
{
    auto conn = co_await server->accept();
    if (!conn.has_value()) co_return;

    std::string pending;
    if (!co_await server->read_head(*conn, pending)) co_return;

    if constexpr (close)
    {
        if (!co_await raw_server::send(*conn, "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok"sv))
            co_return;
        server->after = co_await raw_server::rest(*conn, std::move(pending));
    }
    else
    {
        if (!co_await raw_server::send(*conn, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"sv)) co_return;
        (void)co_await server->read_body(*conn, pending, 5);

        if (!co_await server->read_head(*conn, pending)) co_return;
        (void)co_await raw_server::send(*conn, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nagain"sv);
    }
}

// refuse_after_body answers with 417 once it has the body (which comes when the client's done waiting).
task<> refuse_after_body(raw_server* server)
{
    auto conn = co_await server->accept();
    if (!conn.has_value()) co_return;

    std::string pending;
    if (!co_await server->read_head(*conn, pending)) co_return;
    (void)co_await server->read_body(*conn, pending, 5);
    (void)co_await raw_server::send(*conn, "HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n"sv);
}

// at_once reads a request's body (of the length its head says) without answering first, within a few seconds, and
// echoes it: the client must send it with its head.
task<> at_once(raw_server* server)
{
    auto conn = co_await server->accept();
    if (!conn.has_value()) co_return;

    std::string pending;
    auto        head = co_await server->read_head(*conn, pending);
    if (!head.has_value()) co_return;

    const auto  at     = head->find("Content-Length: ");
    std::size_t length = 0;
    if (at != std::string::npos) length = std::stoul(head->substr(at + 16));

    auto body = co_await server->read_body(*conn, pending, length, net::io::clock::now() + 5s);
    (void)co_await raw_server::send(*conn,
                                    std::format("HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n{}", body.size(), body));
}

bool has_expect(std::string_view head) { return head.contains("\r\nExpect: 100-continue"); }

// exchange sends req, then waits for the server's script to finish.
task<outcome> exchange(http::client* client, raw_server* server, http::client_request req)
{
    auto       out      = co_await send(client, std::move(req));
    const bool finished = co_await server->done();
    CHECK(finished);
    co_return out;
}

}

TEST_CASE("an HTTP/1.1 request that gets 417 instead of 100 is sent again without the expectation",
          "[http][client][expect]")
{
    const bool close = GENERATE(false, true);
    CAPTURE(close);

    order              log;
    net::io::scheduler sched{make_pool()};
    raw_server         server{&sched};
    http::client       client{&sched, options_with(30s)};
    server.run(close ? &expectation_failed<true> : &expectation_failed<false>);

    const auto out = sched.run_to_completion(exchange(&client, &server, post(server.url(), "hello", &log)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.body == "hello");

    // the first had the expectation, and no body; the second, on another connection, not, and its body whole.
    REQUIRE(server.heads.size() == 2);
    CHECK(has_expect(server.heads[0]));
    CHECK_FALSE(has_expect(server.heads[1]));
    CHECK(server.after.empty());
    CHECK(server.bodies == std::vector<std::string>{"hello"});
    CHECK(server.accepted.load() == 2);
    REQUIRE_FALSE(log.seen().empty());
    CHECK(log.seen().front() == "read"); // the 417 isn't an interim response
}

TEST_CASE("an HTTP/1.1 request expecting 100 sends its body once the wait's over", "[http][client][expect]")
{
    // an interim response doesn't put the wait off.
    const bool hint = GENERATE(false, true);
    CAPTURE(hint);

    order              log;
    net::io::scheduler sched{make_pool()};
    raw_server         server{&sched};
    http::client       client{&sched, options_with(50ms)};
    server.run(hint ? &silent<true> : &silent<false>);

    const auto out = sched.run_to_completion(exchange(&client, &server, post(server.url(), "hello", &log)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.body == "hello");
    REQUIRE(server.heads.size() == 1);
    CHECK(has_expect(server.heads[0]));
    CHECK(server.bodies == std::vector<std::string>{"hello"});
    const auto seen = log.seen();
    CAPTURE(seen);
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.front() == (hint ? "103" : "read"));
}

TEST_CASE("an HTTP/1.1 request expecting 100 passes other interim responses on, and goes on waiting",
          "[http][client][expect]")
{
    order              log;
    net::io::scheduler sched{make_pool()};
    raw_server         server{&sched};
    http::client       client{&sched, options_with(30s)};
    server.run(hints_then_continue);

    const auto out = sched.run_to_completion(exchange(&client, &server, post(server.url(), "hello", &log)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.body == "hello");

    // nothing came after the 103, and the body after the 100.
    CHECK(server.quiet);
    CHECK(server.bodies == std::vector<std::string>{"hello"});
    const auto seen = log.seen();
    CAPTURE(seen);
    REQUIRE(seen.size() >= 3);
    CHECK(seen[0] == "103");
    CHECK(seen[1] == "100");
    CHECK(seen[2] == "read");
}

TEST_CASE("an HTTP/1.1 request expecting 100 sends its body after a final response that keeps the connection",
          "[http][client][expect]")
{
    order              log;
    net::io::scheduler sched{make_pool()};
    raw_server         server{&sched};
    http::client       client{&sched, options_with(30s)};
    server.run(answer_first<false>);

    const auto test = [](http::client* client, raw_server* server, order* log) -> task<std::pair<outcome, outcome>>
    {
        auto first = co_await send(client, post(server->url(), "hello", log));

        // the next request, on the same connection.
        auto second = co_await send(client, http::client_request{.uri = net::url::parse(server->url()).value()});

        const bool finished = co_await server->done();
        CHECK(finished);
        co_return std::pair{std::move(first), std::move(second)};
    };

    const auto [first, second] = sched.run_to_completion(test(&client, &server, &log));

    CAPTURE(first.err.message(), second.err.message());
    CHECK(first.status == 200);
    CHECK(first.body == "ok");
    CHECK(second.status == 200);
    CHECK(second.body == "again");

    // the body went (for the framing), and the connection was reused.
    CHECK(server.bodies == std::vector<std::string>{"hello"});
    CHECK(log.reads() > 0);
    CHECK(server.accepted.load() == 1);
}

TEST_CASE("an HTTP/1.1 request expecting 100 doesn't send its body after a final response that closes the connection",
          "[http][client][expect]")
{
    order              log;
    net::io::scheduler sched{make_pool()};
    raw_server         server{&sched};
    http::client       client{&sched, options_with(30s)};
    server.run(answer_first<true>);

    const auto out = sched.run_to_completion(exchange(&client, &server, post(server.url(), "hello", &log)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.body == "ok");
    CHECK(server.after.empty());
    CHECK(log.reads() == 0);
    CHECK(client.stats() == http::connection_stats{});
}

TEST_CASE("an HTTP/1.1 request's 417 after its body was sent is returned", "[http][client][expect]")
{
    order              log;
    net::io::scheduler sched{make_pool()};
    raw_server         server{&sched};
    http::client       client{&sched, options_with(50ms)};
    server.run(refuse_after_body);

    const auto out = sched.run_to_completion(exchange(&client, &server, post(server.url(), "hello", &log)));

    CAPTURE(out.err.message());
    CHECK(out.status == 417);
    CHECK(server.bodies == std::vector<std::string>{"hello"});
    CHECK(server.accepted.load() == 1);
}

TEST_CASE("an HTTP/1.1 request that doesn't wait for 100 sends its body with its head", "[http][client][expect]")
{
    struct test_case
    {
        std::string_view          name;
        bool                      expect  = false;
        std::chrono::milliseconds timeout = 1h;
        std::string_view          body;
    };

    const auto test =
        GENERATE(test_case{.name = "without the expectation", .expect = false, .body = "hello"},
                 test_case{.name = "the wait turned off", .expect = true, .timeout = 0ms, .body = "hello"},
                 test_case{.name = "a body of 0", .expect = true, .body = ""});
    CAPTURE(test.name);

    order              log;
    net::io::scheduler sched{make_pool()};
    raw_server         server{&sched};
    http::client       client{&sched, options_with(test.timeout)};
    server.run(at_once);

    auto req = post(server.url(), test.body, &log);
    if (!test.expect) req.headers.remove("Expect");

    const auto out = sched.run_to_completion(exchange(&client, &server, std::move(req)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.body == test.body);
    CHECK(server.bodies == std::vector<std::string>{std::string{test.body}});
    CHECK(log.seen().empty() == test.body.empty()); // no interim response; the body read, if there's one
}

// Over HTTP/2 (the HTTP/2 plan's stage 6, sixth part).
namespace
{

namespace http2 = net::http::http2;

http::client_options h2_options_with(std::chrono::milliseconds expect_continue_timeout)
{
    auto options                  = options_with(expect_continue_timeout);
    options.http2_prior_knowledge = true;
    return options;
}

// background is a request sent on a task of its own, while the test plays the server: what it came to, once finished.
struct background
{
    outcome                    result;
    std::atomic<std::uint64_t> finished{0};

    // wait waits (up to 5 seconds) for the request to have finished, returning whether it has.
    task<bool> wait(net::io::scheduler* sched) const { return test::stop_probe::reach(sched, finished, 1); }
};

task<> send_in_background(net::io::scheduler* sched, http::client* client, http::client_request req, background* out)
{
    out->result = co_await send(client, std::move(req));
    test::stop_probe::bump(sched, out->finished);
}

// h2_host is a host the client speaks HTTP/2 to (with prior knowledge), whose connection the test accepts, to play the
// server on it with test::h2_client.
struct h2_host
{
    explicit h2_host(net::io::scheduler* sched)
        : listener{net::listener::make(sched, "127.0.0.1", "0").value()}
    {}

    [[nodiscard]] std::string url() const { return std::format("http://127.0.0.1:{}/", listener.local_port()); }

    net::listener listener;
};

// played is a connection the test accepted, and the server it plays on it.
struct played
{
    explicit played(net::tcp_socket accepted)
        : socket{std::move(accepted)}
        , server{&socket, &socket, &socket, http2::role::server}
    {}

    net::tcp_socket socket;
    test::h2_client server;
};

task<std::unique_ptr<played>> accept(h2_host* host)
{
    auto accepted = co_await host->listener.accept();
    REQUIRE(accepted.has_value());
    co_return std::make_unique<played>(std::move(*accepted));
}

// head_in is whether a request's head (stream id) has come, in done()'s form for pump().
auto head_in(test::h2_client& server, http2::stream_id id)
{
    return [&server, id] { return server.responses.contains(id) && !server.responses[id].headers.empty(); };
}

// body_begun is whether any of a request's body (stream id) has come, or it's ended.
auto body_begun(test::h2_client& server, http2::stream_id id)
{
    return [&server, id] { return server.responses[id].data_frames > 0 || server.responses[id].done; };
}

// quiet is whether nothing of a request's body comes for a while: e.g. while it waits for 100 (Continue).
task<bool> quiet(test::h2_client& server, http2::stream_id id, net::io::clock::duration wait)
{
    const bool begun = co_await server.pump(body_begun(server, id), wait);
    co_return !begun && server.timed_out;
}

// answer answers a request (stream id) with 200 and its body (once it's all come), and waits for the client to have
// read it.
task<bool> answer(net::io::scheduler* sched, test::h2_client& server, http2::stream_id id, background* out)
{
    const bool whole = co_await server.wait_for(id);
    if (!whole) co_return false;

    server.respond(id, "200", server.responses[id].body);
    const bool written = co_await server.pump([] { return true; });
    if (!written) co_return false;
    co_return co_await out->wait(sched);
}

}

TEST_CASE("an HTTP/2 request expecting 100 sends its body once the server's 100 comes", "[http][http2][client][expect]")
{
    // the 100 is waited for whether or not the request wants interim responses.
    const bool informational = GENERATE(true, false);
    CAPTURE(informational);

    order              log; // (before the scheduler, whose tasks use it)
    net::io::scheduler sched{make_pool()};

    // a client still waiting fails the handler's read of the body, sooner than the wait would end.
    http::server_config config;
    config.body_read_timeout = 2s;
    served server{&sched, config};

    // long enough that the body can only go for the 100.
    http::client client{&sched, h2_options_with(30s)};

    auto req = post(server.url("/echo"), "hello", &log);
    if (!informational) req.on_informational = nullptr;

    const auto out = sched.run_to_completion(send(&client, std::move(req)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.body == "hello");

    // the 100 (once, if it's passed on), then the body's reads.
    const auto seen = log.seen();
    CAPTURE(seen);
    REQUIRE(seen.size() >= (informational ? 2 : 1));
    CHECK(std::ranges::count(seen, std::string{"100"}) == (informational ? 1 : 0));
    if (informational) CHECK(seen.front() == "100");
    CHECK(seen[informational ? 1 : 0] == "read");
}

TEST_CASE("an HTTP/2 request expecting 100 doesn't send its body if the server answers 413 first",
          "[http][http2][client][expect]")
{
    order              log;
    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, h2_options_with(30s)};

    const auto out = sched.run_to_completion(send(&client, post(server.url("/refuse"), "hello", &log)));

    CAPTURE(out.err.message());
    CHECK(out.status == 413);
    CHECK(out.body.empty());
    CHECK(log.reads() == 0);
    CHECK(log.seen().empty());
}

TEST_CASE("an HTTP/2 request expecting 100 sends its body once the wait's over", "[http][http2][client][expect]")
{
    order              log;
    background         out;
    net::io::scheduler sched{make_pool()};
    h2_host            host{&sched};
    http::client       client{&sched, h2_options_with(300ms)};

    const auto test =
        [](net::io::scheduler* sched, http::client* client, h2_host* host, order* log, background* out) -> task<>
    {
        REQUIRE(sched->spawn(send_in_background(sched, client, post(host->url(), "hello", log), out)));

        auto       conn   = co_await accept(host);
        auto&      server = conn->server;
        const bool in     = co_await server.pump(head_in(server, 1));
        REQUIRE(in);
        CHECK(test::header(server.responses[1], "expect") == "100-continue");

        // nothing for a while (the wait's 300 ms), then the body (the timer's ended the wait).
        const bool waiting = co_await quiet(server, 1, 100ms);
        CHECK(waiting);
        const bool answered = co_await answer(sched, server, 1, out);
        REQUIRE(answered);
        CHECK(server.responses[1].body == "hello");
    };

    sched.run_to_completion(test(&sched, &client, &host, &log, &out));

    CAPTURE(out.result.err.message());
    CHECK(out.result.status == 200);
    CHECK(out.result.body == "hello");
    const auto seen = log.seen();
    CAPTURE(seen);
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.front() == "read"); // no interim response
}

TEST_CASE("an HTTP/2 request expecting 100 passes other interim responses on, and goes on waiting",
          "[http][http2][client][expect]")
{
    order              log;
    background         out;
    net::io::scheduler sched{make_pool()};
    h2_host            host{&sched};
    http::client       client{&sched, h2_options_with(30s)};

    const auto test =
        [](net::io::scheduler* sched, http::client* client, h2_host* host, order* log, background* out) -> task<>
    {
        REQUIRE(sched->spawn(send_in_background(sched, client, post(host->url(), "hello", log), out)));

        auto       conn   = co_await accept(host);
        auto&      server = conn->server;
        const bool in     = co_await server.pump(head_in(server, 1));
        REQUIRE(in);

        // a 103, then nothing for a while: the client still waits.
        server.respond(1, "103", {}, false);
        const bool waiting = co_await quiet(server, 1, 200ms);
        CHECK(waiting);

        server.respond(1, "100", {}, false);
        const bool answered = co_await answer(sched, server, 1, out);
        REQUIRE(answered);
    };

    sched.run_to_completion(test(&sched, &client, &host, &log, &out));

    CAPTURE(out.result.err.message());
    CHECK(out.result.status == 200);
    CHECK(out.result.body == "hello");
    const auto seen = log.seen();
    CAPTURE(seen);
    REQUIRE(seen.size() >= 3);
    CHECK(seen[0] == "103");
    CHECK(seen[1] == "100");
    CHECK(seen[2] == "read");
}

TEST_CASE("an HTTP/2 request expecting 100 sends its body after a 2xx first, once the wait's over",
          "[http][http2][client][expect]")
{
    order              log;
    background         out;
    net::io::scheduler sched{make_pool()};
    h2_host            host{&sched};
    http::client       client{&sched, h2_options_with(300ms)};

    const auto test =
        [](net::io::scheduler* sched, http::client* client, h2_host* host, order* log, background* out) -> task<>
    {
        REQUIRE(sched->spawn(send_in_background(sched, client, post(host->url(), "hello", log), out)));

        auto       conn   = co_await accept(host);
        auto&      server = conn->server;
        const bool in     = co_await server.pump(head_in(server, 1));
        REQUIRE(in);

        // the whole response first: the body still comes, once the wait's over (the stream may be full-duplex).
        server.respond(1, "200", "ok");
        const bool waiting = co_await quiet(server, 1, 100ms);
        CHECK(waiting);
        const bool whole = co_await server.wait_for(1);
        REQUIRE(whole);
        CHECK(server.responses[1].body == "hello");
        CHECK_FALSE(server.responses[1].reset);

        const bool finished = co_await out->wait(sched);
        REQUIRE(finished);
    };

    sched.run_to_completion(test(&sched, &client, &host, &log, &out));

    CAPTURE(out.result.err.message());
    CHECK(out.result.status == 200);
    CHECK(out.result.body == "ok");
    CHECK(log.reads() > 0);
}

TEST_CASE("an HTTP/2 request expecting 100 doesn't send its body after a final response above 299 first",
          "[http][http2][client][expect]")
{
    order              log;
    background         out;
    net::io::scheduler sched{make_pool()};
    h2_host            host{&sched};
    http::client       client{&sched, h2_options_with(30s)};

    const auto test =
        [](net::io::scheduler* sched, http::client* client, h2_host* host, order* log, background* out) -> task<>
    {
        REQUIRE(sched->spawn(send_in_background(sched, client, post(host->url(), "hello", log), out)));

        auto       conn   = co_await accept(host);
        auto&      server = conn->server;
        const bool in     = co_await server.pump(head_in(server, 1));
        REQUIRE(in);

        server.respond(1, "403", "no");
        const bool written = co_await server.pump([] { return true; });
        REQUIRE(written);

        // the response is read whole, then dropped: the stream's reset, as the request never ended, without any of
        // its body.
        const bool finished = co_await out->wait(sched);
        REQUIRE(finished);
        const bool reset = co_await server.wait_for(1);
        REQUIRE(reset);
        CHECK(server.responses[1].reset == http2::error_code::cancel);
        CHECK(server.responses[1].data_frames == 0);
    };

    sched.run_to_completion(test(&sched, &client, &host, &log, &out));

    CAPTURE(out.result.err.message());
    CHECK(out.result.status == 403);
    CHECK(out.result.body == "no");
    CHECK(log.reads() == 0);
}

TEST_CASE("an HTTP/2 request that gets 417 instead of 100 is sent again without the expectation",
          "[http][http2][client][expect]")
{
    order              log;
    background         out;
    net::io::scheduler sched{make_pool()};
    h2_host            host{&sched};
    http::client       client{&sched, h2_options_with(30s)};

    const auto test =
        [](net::io::scheduler* sched, http::client* client, h2_host* host, order* log, background* out) -> task<>
    {
        REQUIRE(sched->spawn(send_in_background(sched, client, post(host->url(), "hello", log), out)));

        auto       conn   = co_await accept(host);
        auto&      server = conn->server;
        const bool in     = co_await server.pump(head_in(server, 1));
        REQUIRE(in);

        server.respond(1, "417");

        // again, on the same connection (a new stream), with its body at once; the first reset without any.
        const bool again = co_await server.pump(head_in(server, 3));
        REQUIRE(again);
        CHECK(test::header(server.responses[3], "expect") == "(none)");
        const bool answered = co_await answer(sched, server, 3, out);
        REQUIRE(answered);
        CHECK(server.responses[3].body == "hello");

        CHECK(server.responses[1].reset == http2::error_code::cancel);
        CHECK(server.responses[1].data_frames == 0);
    };

    sched.run_to_completion(test(&sched, &client, &host, &log, &out));

    CAPTURE(out.result.err.message());
    CHECK(out.result.status == 200);
    CHECK(out.result.body == "hello");
    REQUIRE_FALSE(log.seen().empty());
    CHECK(log.seen().front() == "read"); // the 417 isn't an interim response
}

TEST_CASE("an HTTP/2 request that doesn't wait for 100 sends its body with its head", "[http][http2][client][expect]")
{
    struct test_case
    {
        std::string_view          name;
        bool                      expect  = false;
        std::chrono::milliseconds timeout = 1h;
        std::string_view          body;
    };

    const auto tc = GENERATE(test_case{.name = "without the expectation", .expect = false, .body = "hello"},
                             test_case{.name = "the wait turned off", .expect = true, .timeout = 0ms, .body = "hello"},
                             test_case{.name = "a body of 0", .expect = true, .body = ""});
    CAPTURE(tc.name);

    order              log;
    background         out;
    net::io::scheduler sched{make_pool()};
    h2_host            host{&sched};
    http::client       client{&sched, h2_options_with(tc.timeout)};

    auto req = post(host.url(), tc.body, &log);
    if (!tc.expect) req.headers.remove("Expect");

    const auto test = [](net::io::scheduler*  sched,
                         http::client*        client,
                         h2_host*             host,
                         http::client_request req,
                         background*          out) -> task<>
    {
        const bool spawned = sched->spawn(send_in_background(sched, client, std::move(req), out));
        REQUIRE(spawned);

        // the whole request, without anything from the server first.
        auto       conn     = co_await accept(host);
        const bool answered = co_await answer(sched, conn->server, 1, out);
        REQUIRE(answered);
    };

    sched.run_to_completion(test(&sched, &client, &host, std::move(req), &out));

    CAPTURE(out.result.err.message());
    CHECK(out.result.status == 200);
    CHECK(out.result.body == tc.body);
    CHECK(log.seen().empty() == tc.body.empty()); // no interim response; the body read, if there's one
}
