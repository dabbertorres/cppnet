#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
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
#include "http/hpack.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/string_reader.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "url.hpp"
#include "util/string_util.hpp"

// The client's trailers: a response's, which client_response::trailers() has once its body's been read to its end, and
// a request's, which client_request::make_trailers fills once its body's been read, over HTTP/1.1 and HTTP/2, against
// cppnet's server, or a server the test plays.
namespace
{

namespace http  = net::http;
namespace http2 = net::http::http2;
namespace hpack = net::http::hpack;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;

// describe is fields as "name=value;...", names lowercased (as HTTP/2 sends them), in order.
std::string describe(http::fields_view fields)
{
    std::string out;
    for (const auto& [name, values] : fields)
    {
        for (const auto& value : values)
        {
            for (const char c : name) out.push_back(net::util::to_lower_ascii(c));
            out.append("=").append(value).append(";");
        }
    }
    return out;
}

// list is a list-based field's members, joined by commas.
std::string list(const http::headers& fields, std::string_view name)
{
    std::string out;
    for (const auto member : fields.get_list(name)) out.append(out.empty() ? "" : ",").append(member);
    return out;
}

// probe is what a handler saw of a request's body, which a test waits for.
struct probe
{
    std::atomic<std::uint64_t> done{0};
    std::string                body;
    std::error_code            err;

    // wait waits (up to 5 seconds) for the handler to have seen it, returning whether it has.
    task<bool> wait(net::io::scheduler* sched)
    {
        const auto deadline = net::io::clock::now() + 5s;
        while (done.load() == 0)
        {
            if (co_await sched->wait_on(done, 0, deadline)) break;
        }
        co_return done.load() != 0;
    }
};

// echo answers with the request's body and trailers: "body|name=value;...", with a Content-Length.
task<void> echo(http::server_request& req, http::response_writer& resp)
{
    std::string body;
    if (req.body != nullptr)
    {
        auto read = co_await net::io::read_all(*req.body);
        if (!read.has_value()) throw std::system_error{read.error()};
        body = std::move(*read);
    }

    const auto msg = body + "|" + describe(req.trailers);
    auto*      out = co_await resp.send(http::status::OK, msg.size());
    co_await net::io::write_all(*out, msg);
}

// served is cppnet's server on 127.0.0.1, with routes for the tests.
struct served
{
    explicit served(net::io::scheduler* sched)
        : server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), routes(sched)}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    [[nodiscard]] std::string url(std::string_view path) const
    {
        return std::format("http://127.0.0.1:{}{}", server.local_port(), path);
    }

    http::router routes(net::io::scheduler* sched)
    {
        http::router r;

        // a body of unknown length (chunked, over HTTP/1.1), then trailers (the server drops the one that can't be).
        r.GET("/chunked",
              [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
              {
                  auto* body = co_await resp.send(http::status::OK);
                  co_await net::io::write_all(*body, "hello"sv);
                  resp.trailers().set("Server-Timing", "db;dur=53");
                  resp.trailers().set("Grpc-Status", "0");
                  resp.trailers().set("Content-Type", "text/plain");
              });

        // a body with a Content-Length, then trailers, declared (Trailer) before its head's sent: only HTTP/2 sends
        // them.
        r.GET("/length",
              [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
              {
                  resp.headers().set("Trailer", "Server-Timing, Grpc-Status");
                  auto* body = co_await resp.send(http::status::OK, 5);
                  co_await net::io::write_all(*body, "hello"sv);
                  resp.trailers().set("Server-Timing", "db;dur=53");
                  resp.trailers().set("Grpc-Status", "0");
              });

        // a body with a Content-Length, then trailers not declared: dropped over HTTP/2 too, as its last DATA ends
        // the stream.
        r.GET("/undeclared",
              [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
              {
                  auto* body = co_await resp.send(http::status::OK, 5);
                  co_await net::io::write_all(*body, "hello"sv);
                  resp.trailers().set("Server-Timing", "db;dur=53");
                  resp.trailers().set("Grpc-Status", "0");
              });

        // trailers, without a body: only HTTP/2 sends them.
        r.GET("/empty",
              [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
              {
                  resp.trailers().set("Server-Timing", "db;dur=53");
                  resp.trailers().set("Grpc-Status", "0");
                  co_await resp.send(http::status::OK, 0);
              });

        // a body of unknown length with other trailers.
        r.GET("/other",
              [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
              {
                  auto* body = co_await resp.send(http::status::OK);
                  co_await net::io::write_all(*body, "other"sv);
                  resp.trailers().set("X-Second", "2");
              });

        // a body of unknown length without trailers.
        r.GET("/plain",
              [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
              {
                  auto* body = co_await resp.send(http::status::OK);
                  co_await net::io::write_all(*body, "plain"sv);
              });

        r.GET("/echo", echo);
        r.POST("/echo", echo);

        // the request's body read, then a redirect to /echo (307 or 303).
        const auto redirect = [](http::status code)
        {
            return [code](http::server_request& req, http::response_writer& resp) -> task<void>
            {
                (void)co_await net::io::read_all(*req.body);
                resp.headers().set("Location", "/echo");
                (void)co_await resp.send(code, 0);
            };
        };
        r.POST("/307", redirect(http::status::TEMPORARY_REDIRECT));
        r.POST("/303", redirect(http::status::SEE_OTHER));

        // the request's body read, and what came of it recorded in sunk.
        r.POST("/sink",
               [this, sched](http::server_request& req, http::response_writer& resp) -> task<void>
               {
                   auto read = co_await net::io::read_all(*req.body);
                   if (read.has_value()) sunk.body = std::move(*read);
                   else sunk.err = read.error();
                   sunk.done.store(1);
                   sched->notify_all(sunk.done);

                   if (read.has_value()) (void)co_await resp.send(http::status::OK, 0);
               });

        // the request's TE and Connection fields: "te=...|connection=...".
        r.GET("/te",
              [](const http::server_request& req, http::response_writer& resp) -> task<void>
              {
                  const auto msg =
                      std::format("te={}|connection={}", list(req.headers, "TE"), list(req.headers, "Connection"));
                  auto* out = co_await resp.send(http::status::OK, msg.size());
                  co_await net::io::write_all(*out, msg);
              });

        return r;
    }

    probe        sunk; // what /sink saw
    http::server server;
};

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

http::client_options options_for(bool http2)
{
    http::client_options options;
    options.http2_prior_knowledge = http2;
    return options;
}

http::client_request request(http::request_method method, const std::string& url)
{
    return {.method = method, .uri = net::url::parse(url).value()};
}

// outcome is what a request came to: its response's status and body, and its trailers (before the body was read, and
// after), or the error.
struct outcome
{
    int             status = 0;
    std::string     body;
    std::string     before; // the trailers before any of the body was read
    std::string     early;  // the trailers once some of it was
    std::string     trailers;
    std::string     headers;
    std::error_code err;
};

// send sends req, and reads its response's body to its end, a byte first.
task<outcome> send(http::client* client, http::client_request req)
{
    outcome out;

    auto resp = co_await client->send(std::move(req));
    if (!resp.has_value())
    {
        out.err = resp.error();
        co_return out;
    }

    out.status  = static_cast<int>(resp->status_code);
    out.headers = describe(resp->headers);
    out.before  = describe(resp->trailers());

    std::array<std::byte, 1> first{};
    const auto               res = co_await resp->body->read(first);
    out.body.append(reinterpret_cast<const char*>(first.data()), res.count);
    out.early = describe(resp->trailers());

    if (res.err != net::io::errc::end_of_stream)
    {
        if (res.err)
        {
            out.err = res.err;
            co_return out;
        }

        auto rest = co_await net::io::read_all(*resp->body);
        if (!rest.has_value())
        {
            out.err = rest.error();
            co_return out;
        }
        out.body += *rest;
    }

    out.trailers = describe(resp->trailers());
    co_return out;
}

// chunked_body is a body of unknown length (sent chunked over HTTP/1.1).
std::unique_ptr<net::io::reader> chunked_body(std::string content)
{
    return std::make_unique<net::io::owned_string_reader>(std::move(content));
}

// add_digest is a make_trailers that counts its calls, adding a trailer that may be, and one that can't (dropped).
auto add_digest(int* calls)
{
    return [calls](http::trailer_fields& trailers)
    {
        ++*calls;
        trailers.set("X-Digest", "abc");
        trailers.set("Content-Type", "text/plain");
    };
}

// background is what a request sent in the background came to, for a test that plays the server meanwhile.
struct background
{
    std::atomic<std::uint64_t> finished{0};
    outcome                    out;

    task<bool> wait(net::io::scheduler* sched)
    {
        const auto deadline = net::io::clock::now() + 5s;
        while (finished.load() == 0)
        {
            if (co_await sched->wait_on(finished, 0, deadline)) break;
        }
        co_return finished.load() != 0;
    }
};

task<> send_in_background(net::io::scheduler* sched, http::client* client, http::client_request req, background* bg)
{
    bg->out = co_await send(client, std::move(req));
    bg->finished.store(1);
    sched->notify_all(bg->finished);
}

// read_head reads a request's head from conn, returning whether it all came.
task<bool> read_head(net::tcp_socket& conn)
{
    std::string                head;
    std::array<std::byte, 512> buf{};
    while (!head.contains("\r\n\r\n"))
    {
        auto res = co_await conn.read(buf);
        head.append(reinterpret_cast<const char*>(buf.data()), res.count);
        if (res.err) co_return false;
    }
    co_return true;
}

}

TEST_CASE("a response's trailers are the client's once its body's been read to its end", "[http][client][trailers]")
{
    struct test_case
    {
        std::string_view path;
        bool             http2;
        bool             has_trailers; // a chunked body has them over HTTP/1.1; over HTTP/2, any whose last DATA
                                       // doesn't end the stream
        std::string_view body;
    };

    const auto tc = GENERATE(test_case{.path = "/chunked", .http2 = false, .has_trailers = true, .body = "hello"},
                             test_case{.path = "/chunked", .http2 = true, .has_trailers = true, .body = "hello"},
                             test_case{.path = "/length", .http2 = false, .has_trailers = false, .body = "hello"},
                             test_case{.path = "/length", .http2 = true, .has_trailers = true, .body = "hello"},
                             test_case{.path = "/undeclared", .http2 = false, .has_trailers = false, .body = "hello"},
                             test_case{.path = "/undeclared", .http2 = true, .has_trailers = false, .body = "hello"},
                             test_case{.path = "/empty", .http2 = false, .has_trailers = false, .body = ""},
                             test_case{.path = "/empty", .http2 = true, .has_trailers = true, .body = ""});
    CAPTURE(tc.path, tc.http2);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(tc.http2)};

    const auto out = sched.run_to_completion(send(&client, request(http::request_method::GET, server.url(tc.path))));

    CAPTURE(out.err.message());
    REQUIRE_FALSE(out.err);
    CHECK(out.status == 200);
    CHECK(out.body == tc.body);

    // only once the body's end has been read: not before (though they may have arrived), nor part way.
    CHECK(out.before.empty());
    if (!tc.body.empty()) CHECK(out.early.empty());

    // apart from the headers, and without the one that can't be a trailer (which the server drops).
    CHECK(out.trailers == (tc.has_trailers ? "server-timing=db;dur=53;grpc-status=0;" : ""));
    CHECK_FALSE(out.headers.contains("server-timing"));
}

TEST_CASE("a response's trailers that can't be trailers are dropped", "[http][client][trailers]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    net::io::scheduler sched{make_pool()};
    auto               listener = net::listener::make(&sched, "127.0.0.1", "0").value();
    http::client       client{&sched, options_for(http2)};

    // the test plays the server: cppnet's drops those itself.
    const auto test = [](net::io::scheduler* sched, http::client* client, net::listener* listener, bool http2) -> task<>
    {
        background bg;
        REQUIRE(sched->spawn(send_in_background(
            sched,
            client,
            request(http::request_method::GET, std::format("http://127.0.0.1:{}/", listener->local_port())),
            &bg)));

        auto conn = co_await listener->accept();
        REQUIRE(conn.has_value());

        if (http2)
        {
            test::h2_client server{&*conn, &*conn, &*conn, http2::role::server};

            const bool requested = co_await server.pump([&] { return server.responses.contains(1); });
            REQUIRE(requested);

            server.respond(1, "200", "hello", false);
            const std::vector<hpack::field> trailers{
                {.name = "content-length", .value = "5"},
                {      .name = "if-match", .value = "y"},
                { .name = "authorization", .value = "z"},
                { .name = "cache-control", .value = "x"},
                {          .name = "x-ok", .value = "1"},
            };
            server.send_trailers(1, trailers);
            const bool written = co_await server.pump([] { return true; }); // (just written)
            REQUIRE(written);

            // NOTE: the connection's kept until the client's done with it.
            const bool sent = co_await bg.wait(sched);
            REQUIRE(sent);
        }
        else
        {
            const bool head = co_await read_head(*conn);
            REQUIRE(head);
            const auto response =
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n"
                "Content-Length: 5\r\nHost: x\r\nIf-Match: y\r\nAuthorization: z\r\nX-Ok: 1\r\n\r\n"sv;
            const auto wrote = co_await net::io::write_all(*conn, response);
            REQUIRE_FALSE(wrote.err);
            const bool sent = co_await bg.wait(sched);
            REQUIRE(sent);
        }

        CAPTURE(bg.out.err.message());
        CHECK(bg.out.status == 200);
        CHECK(bg.out.body == "hello");
        CHECK(bg.out.trailers == "x-ok=1;");
    };

    sched.run_to_completion(test(&sched, &client, &listener, http2));
}

TEST_CASE("make_trailers' fields reach the handler", "[http][client][trailers]")
{
    enum class body_kind : std::uint8_t
    {
        chunked, // of unknown length
        length,  // with a Content-Length
        none,
    };

    const bool http2 = GENERATE(false, true);
    const auto kind  = GENERATE(body_kind::chunked, body_kind::length, body_kind::none);
    CAPTURE(http2, kind);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(http2)};

    int  calls = 0;
    auto req   = request(http::request_method::POST, server.url("/echo"));
    if (kind == body_kind::chunked) req.body = chunked_body("hello");
    if (kind == body_kind::length) req.set_body("hello");
    req.make_trailers = add_digest(&calls);

    const auto out = sched.run_to_completion(send(&client, std::move(req)));

    // HTTP/2 sends them whatever the body (after the head, without one); HTTP/1.1 only after a chunked body.
    const bool sent = http2 || kind == body_kind::chunked;

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    CHECK(out.body == std::format("{}|{}", kind == body_kind::none ? "" : "hello", sent ? "x-digest=abc;" : ""));
    CHECK(calls == (sent ? 1 : 0));
}

TEST_CASE("a redirect that sends the body again calls make_trailers again", "[http][client][trailers][redirect]")
{
    const bool http2  = GENERATE(false, true);
    const auto status = GENERATE(307, 303);
    CAPTURE(http2, status);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};

    auto options          = options_for(http2);
    options.max_redirects = 5;
    http::client client{&sched, options};

    int  calls        = 0;
    auto req          = request(http::request_method::POST, server.url(std::format("/{}", status)));
    req.body          = chunked_body("hello");
    req.get_body      = [] { return chunked_body("hello"); };
    req.make_trailers = add_digest(&calls);

    const auto out = sched.run_to_completion(send(&client, std::move(req)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);
    if (status == 307)
    {
        // the body, and its trailers, again.
        CHECK(out.body == "hello|x-digest=abc;");
        CHECK(calls == 2);
    }
    else
    {
        // a GET, without the body, so nor its trailers.
        CHECK(out.body == "|");
        CHECK(calls == 1);
    }
}

TEST_CASE("a trailer that can't be sent fails the request, cut short", "[http][client][trailers]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(http2)};

    const auto test = [](net::io::scheduler* sched, http::client* client, served* server) -> task<>
    {
        // more than the client's buffer, so the server has the head (and handler) before the request fails.
        auto req          = request(http::request_method::POST, server->url("/sink"));
        req.body          = chunked_body(std::string(100'000, 'x'));
        req.make_trailers = [](http::trailer_fields& trailers) { trailers.set("X-Bad", "a\r\nb"); };

        const auto out = co_await send(client, std::move(req));
        CHECK(out.err == std::errc::invalid_argument);

        // the server didn't take it as complete: its connection was closed (HTTP/1.1), or its stream reset (HTTP/2).
        const bool sunk = co_await server->sunk.wait(sched);
        REQUIRE(sunk);
        CHECK(server->sunk.err);
        CHECK(server->sunk.body.empty());

        // and the client goes on.
        const auto next = co_await send(client, request(http::request_method::GET, server->url("/plain")));
        CAPTURE(next.err.message());
        CHECK(next.body == "plain");
    };

    sched.run_to_completion(test(&sched, &client, &server));
}

TEST_CASE("TE: trailers set by the caller is sent, as each version has it", "[http][client][trailers][te]")
{
    const bool http2          = GENERATE(false, true);
    const auto te             = GENERATE("trailers"sv, "gzip"sv);
    const bool connection_set = GENERATE(false, true); // the caller lists TE in Connection itself
    CAPTURE(http2, te, connection_set);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(http2)};

    auto req = request(http::request_method::GET, server.url("/te"));
    req.headers.set("TE", te);
    if (connection_set) req.headers.set("Connection", "TE");

    const auto out = sched.run_to_completion(send(&client, std::move(req)));

    CAPTURE(out.err.message());
    CHECK(out.status == 200);

    // HTTP/1.1: as given, TE listed in Connection (once); HTTP/2: only TE: trailers, and no Connection.
    if (http2) CHECK(out.body == std::format("te={}|connection=", te == "trailers"sv ? "trailers"sv : ""sv));
    else CHECK(out.body == std::format("te={}|connection=TE", te));
}

TEST_CASE("a body the caller put in place of the client's has no trailers", "[http][client][trailers]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(http2)};

    const auto test = [](http::client* client, std::string url) -> task<std::string>
    {
        auto resp = co_await client->send(request(http::request_method::GET, url));
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        auto body = co_await net::io::read_all(*resp->body);
        if (!body.has_value()) co_return "error: " + body.error().message();

        auto out = describe(resp->trailers()) + " / ";

        resp->body = std::make_unique<net::io::owned_string_reader>("mine");
        out += describe(resp->trailers()) + " / ";

        resp->body = nullptr;
        co_return out + describe(resp->trailers());
    };

    CHECK(sched.run_to_completion(test(&client, server.url("/chunked")))
          == "server-timing=db;dur=53;grpc-status=0; /  / ");
}

TEST_CASE("a connection's next response after one with trailers has its own", "[http][client][trailers]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    net::io::scheduler sched{make_pool()};
    served             server{&sched};
    http::client       client{&sched, options_for(http2)};

    const auto test = [](http::client* client, served* server) -> task<std::vector<std::string>>
    {
        std::vector<std::string> out;

        // the first response is kept (with its trailers) as the others are read.
        auto first = co_await client->send(request(http::request_method::GET, server->url("/chunked")));
        REQUIRE(first.has_value());
        const auto first_body = co_await net::io::read_all(*first->body);
        REQUIRE(first_body.has_value());
        out.push_back(describe(first->trailers()));

        for (const auto* path : {"/other", "/plain"})
        {
            auto resp = co_await client->send(request(http::request_method::GET, server->url(path)));
            REQUIRE(resp.has_value());
            const auto resp_body = co_await net::io::read_all(*resp->body);
            REQUIRE(resp_body.has_value());
            out.push_back(describe(resp->trailers()));
        }

        out.push_back(describe(first->trailers()));

        // one connection, reused for each.
        CHECK(client->stats().open == 1);
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&client, &server));
    CHECK(out
          == std::vector<std::string>{"server-timing=db;dur=53;grpc-status=0;",
                                      "x-second=2;",
                                      "",
                                      "server-timing=db;dur=53;grpc-status=0;"});
}
