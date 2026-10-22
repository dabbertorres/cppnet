// The HTTP/2 client's Origin Sets (RFC 8336; docs/plans/http2.md, stage 6's eleventh part): each connection over TLS
// keeps one, from the server's ORIGIN frames, which the server the tests play (test::h2_client, in its server role over
// a TLS stream with a certificate for a.test, b.test, *.w.test and 127.0.0.1) sends; and the server's certificate
// checked for each origin's host before the connection's taken to be authoritative for it.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
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
#include "http/connection.hpp"
#include "http/connection_pool.hpp"
#include "http/h2_client.hpp"
#include "http/http2_client.hpp"
#include "http/http2_config.hpp"
#include "http/http2_session.hpp"
#include "http/origin.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "tls/test_pki.hpp"
#include "tls/tls.hpp"
#include "url.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;
namespace tls   = net::tls;

using namespace std::chrono_literals;
using net::coro::task;
using test::pki;
using test::server_with;

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// transport is how the client's connection reaches the server: over TLS (verifying its certificate, or not), or
// cleartext (with prior knowledge).
enum class transport : std::uint8_t
{
    tls,
    insecure_tls,
    cleartext,
};

// told counts a connection's origins_known calls, for a test to wait for.
struct told
{
    net::io::scheduler*        sched = nullptr;
    std::atomic<std::uint64_t> count{0};

    // wait waits (up to 5 seconds) for a call, returning whether there's been one.
    task<bool> wait()
    {
        const auto deadline = net::io::clock::now() + 5s;
        while (count.load() == 0)
        {
            if (co_await sched->wait_on(count, 0, deadline)) break;
        }
        co_return count.load() != 0;
    }
};

// connected is a client's HTTP/2 connection to a.test, and the server's side of it.
struct connected
{
    std::unique_ptr<http::detail::connection>              server;
    std::shared_ptr<http::detail::http2_client_connection> conn;
    std::uint16_t                                          port = 0;
};

// handshake is the server's TLS handshake, run as a task of its own while the client's runs.
struct handshake
{
    explicit handshake(net::io::scheduler* sched)
        : sched{sched}
    {}

    net::io::scheduler*                                        sched;
    std::optional<std::expected<tls::stream, std::error_code>> result;
    std::atomic<std::uint64_t>                                 done{0};

    static task<> run(handshake* self, net::tcp_socket accepted)
    {
        self->result = co_await tls::stream::accept(std::move(accepted),
                                                    server_with(pki().names_chain, pki().names_key),
                                                    net::io::clock::now() + 5s,
                                                    {.alpn = std::vector<std::string>{"h2"}});
        self->done.store(1);
        self->sched->notify_all(self->done);
    }

    task<> wait()
    {
        const auto deadline = net::io::clock::now() + 10s;
        while (done.load() == 0)
        {
            if (co_await sched->wait_on(done, 0, deadline)) co_return;
        }
    }
};

// connect connects a client's HTTP/2 connection to the server the test plays, as a.test (its own origin given as
// own_host's, with the port), telling of its Origin Set to known.
task<connected> connect(net::io::scheduler* sched, transport how, told* known, std::string own_host = "a.test")
{
    auto       listener = net::listener::make(sched, "127.0.0.1", "0").value();
    const auto port     = listener.local_port();
    auto       client   = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(port));
    REQUIRE(client.has_value());
    auto accepted = co_await listener.accept();
    REQUIRE(accepted.has_value());

    connected                                 out{.server = nullptr, .conn = nullptr, .port = port};
    std::unique_ptr<http::detail::connection> client_side;
    if (how == transport::cleartext)
    {
        out.server  = std::make_unique<http::detail::connection>(std::move(*accepted));
        client_side = std::make_unique<http::detail::connection>(std::move(*client));
    }
    else
    {
        handshake server{sched};
        REQUIRE(sched->spawn(handshake::run(&server, std::move(*accepted))));

        auto ctx = tls::client_context::make({.trusted_ca_file            = pki().ca,
                                              .insecure_skip_verification = how == transport::insecure_tls,
                                              .alpn                       = {"h2"}});
        REQUIRE(ctx.has_value());
        auto stream = co_await tls::stream::connect(std::move(*client), *ctx, "a.test", net::io::clock::now() + 5s);
        co_await server.wait();

        REQUIRE(stream.has_value());
        REQUIRE(server.result.has_value());
        REQUIRE(server.result->has_value());
        out.server  = std::make_unique<http::detail::connection>(std::move(**server.result));
        client_side = std::make_unique<http::detail::connection>(std::move(*stream));
    }

    http::detail::http2_client_connection::callbacks on{
        .closed = nullptr,
        .freed  = nullptr,
        .origins_known =
            [known](http::detail::http2_client_connection& /*conn*/)
        {
            known->count.fetch_add(1);
            known->sched->notify_all(known->count);
        },
    };
    out.conn = std::make_shared<http::detail::http2_client_connection>(
        sched,
        std::move(client_side),
        http::detail::http2_client_options{},
        std::move(on),
        http::detail::origin_text(own_host, std::to_string(port)));
    REQUIRE(out.conn->start());
    co_return out;
}

// played is the server the test plays, over a connection's server side.
test::h2_client played(http::detail::connection& server)
{
    return test::h2_client{&http::detail::as_reader(server),
                           &http::detail::as_writer(server),
                           &http::detail::socket_of(server),
                           http2::role::server};
}

// send sends ORIGIN with origins, then makes sure the client's had it, and the receive it came in is done with (with
// its callbacks): a PING after it acknowledged twice (the client's reader acknowledges the first as it receives the
// ORIGIN, maybe before it's done with that receive; the second, it receives after).
task<bool> send(test::h2_client& server, std::vector<std::string> origins)
{
    if (server.s.submit_origin(origins)) co_return false;
    if (!co_await server.sync()) co_return false;
    co_return co_await server.sync();
}

// own is the client's own origin, as its Origin Set has it.
std::string own(std::uint16_t port) { return "https://a.test:" + std::to_string(port); }

}

TEST_CASE("an HTTP/2 client's connection over TLS keeps an Origin Set from the server's ORIGIN frames",
          "[tls][http][client][http2][origin]")
{
    told known;

    net::io::scheduler sched{make_pool()};
    known.sched = &sched;

    struct seen
    {
        std::optional<std::vector<std::string>> before{}; // before ORIGIN
        bool                                    told_before = false;
        std::optional<std::vector<std::string>> first{}; // after the first
        bool                                    told_first = false;
        std::optional<std::vector<std::string>> later{}; // after a later one
        std::uint64_t                           told_count = 0;
        std::uint16_t                           port       = 0;
    };

    const auto out = sched.run_to_completion(
        [](net::io::scheduler* sched, told* known) -> task<seen>
        {
            // its own origin as the pool gives it: the host as the URL had it.
            auto c      = co_await connect(sched, transport::tls, known, "A.Test");
            auto server = played(*c.server);

            seen out{.port = c.port};

            // uninitialized until the server sends ORIGIN.
            const bool synced = co_await server.sync();
            REQUIRE(synced);
            out.before      = c.conn->origins();
            out.told_before = known->count.load() != 0;

            // initialized with its own origin and the entries: normalized (lowercase, without 443, an address as
            // ip_addr writes it), each once; what isn't an https origin ignored.
            const bool first = co_await send(server,
                                             {"https://b.test",
                                              "https://C.TEST:443",
                                              "https://x.w.test:8443",
                                              "not an origin",
                                              "http://d.test",
                                              "https://b.test",
                                              "https://*.w.test",
                                              "https://b.test/",
                                              "https://user@b.test",
                                              "https://[0:0::1]",
                                              "https://127.0.0.1:443",
                                              "",
                                              "https://B.test:443"});
            REQUIRE(first);
            out.first      = c.conn->origins();
            out.told_first = co_await known->wait();

            // later ones add to it; told once only.
            const bool later = co_await send(server, {"https://d.test", "https://c.test", "https://e.test:8443"});
            REQUIRE(later);
            out.later      = c.conn->origins();
            out.told_count = known->count.load();

            c.conn->close();
            co_return out;
        }(&sched, &known));

    CHECK_FALSE(out.before.has_value());
    CHECK_FALSE(out.told_before);

    const std::vector<std::string> first{own(out.port),
                                         "https://b.test",
                                         "https://c.test",
                                         "https://x.w.test:8443",
                                         "https://[::1]",
                                         "https://127.0.0.1"};
    REQUIRE(out.first.has_value());
    CHECK(*out.first == first);
    CHECK(out.told_first);

    auto later = first;
    later.emplace_back("https://d.test");
    later.emplace_back("https://e.test:8443");
    REQUIRE(out.later.has_value());
    CHECK(*out.later == later);
    CHECK(out.told_count == 1);
}

TEST_CASE("an HTTP/2 client's connection is authoritative for an origin in its Origin Set, if the server's certificate "
          "is valid for its host",
          "[tls][http][client][http2][origin]")
{
    told known;

    net::io::scheduler sched{make_pool()};
    known.sched = &sched;

    struct answer
    {
        std::string origin;
        bool        authoritative = false;
    };
    struct seen
    {
        bool                before = true; // asked before ORIGIN
        std::vector<answer> answers{};
        std::size_t         checks       = 0; // certificate checks, after the first round
        std::size_t         checks_again = 0; // and after asking again
        std::uint16_t       port         = 0;
    };

    const auto out = sched.run_to_completion(
        [](net::io::scheduler* sched, told* known) -> task<seen>
        {
            auto c      = co_await connect(sched, transport::tls, known);
            auto server = played(*c.server);

            seen out{.port = c.port};

            const bool synced = co_await server.sync();
            REQUIRE(synced);
            out.before = c.conn->authoritative_for("https://a.test:" + std::to_string(c.port));

            const bool sent = co_await send(
                server,
                {"https://b.test", "https://c.test", "https://x.w.test:8443", "https://127.0.0.1", "https://[::1]"});
            REQUIRE(sent);

            const std::vector<std::string> asked{
                "https://a.test:" + std::to_string(c.port), // its own
                "https://b.test",                           // the certificate has it
                "https://B.TEST:443",                       // the same, normalized
                "https://b.test:8443",                      // not in the set (though the certificate has its host)
                "https://c.test",                           // in the set, but the certificate doesn't have it
                "https://x.w.test:8443",                    // the certificate's wildcard has it
                "https://127.0.0.1",                        // the certificate has the address
                "https://[::1]",                            // in the set, but the certificate hasn't the address
                "https://e.test",                           // not in the set, nor the certificate
                "not an origin",
            };
            for (const auto& origin : asked)
                out.answers.push_back({.origin = origin, .authoritative = c.conn->authoritative_for(origin)});
            out.checks = c.conn->certificate_checks();

            // each host's checked once.
            for (const auto& origin : asked) (void)c.conn->authoritative_for(origin);
            out.checks_again = c.conn->certificate_checks();

            c.conn->close();
            co_return out;
        }(&sched, &known));

    CHECK_FALSE(out.before);

    const std::vector<bool> want{true, true, true, false, false, true, true, false, false, false};
    REQUIRE(out.answers.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i)
    {
        CAPTURE(out.answers[i].origin);
        CHECK(out.answers[i].authoritative == want[i]);
    }

    // a.test, b.test, c.test, x.w.test, 127.0.0.1 and ::1: once each.
    CHECK(out.checks == 6);
    CHECK(out.checks_again == 6);
}

TEST_CASE("an HTTP/2 client's Origin Set keeps at most 256 origins", "[tls][http][client][http2][origin]")
{
    told known;

    net::io::scheduler sched{make_pool()};
    known.sched = &sched;

    struct seen
    {
        std::optional<std::vector<std::string>> first; // after 300 entries
        std::optional<std::vector<std::string>> full;  // after another, while it's full
        bool                                    removed = false;
        std::optional<std::vector<std::string>> room; // after another, once there's room
    };

    const auto out = sched.run_to_completion(
        [](net::io::scheduler* sched, told* known) -> task<seen>
        {
            auto c      = co_await connect(sched, transport::tls, known);
            auto server = played(*c.server);

            std::vector<std::string> many;
            many.reserve(300);
            for (int i = 0; i < 300; ++i) many.push_back("https://h" + std::to_string(i) + ".test");

            seen       out;
            const bool first = co_await send(server, many);
            REQUIRE(first);
            out.first = c.conn->origins();

            const bool full = co_await send(server, {"https://more.test"});
            REQUIRE(full);
            out.full = c.conn->origins();

            out.removed     = c.conn->remove_origin("https://h0.test");
            const bool room = co_await send(server, {"https://more.test", "https://most.test"});
            REQUIRE(room);
            out.room = c.conn->origins();

            c.conn->close();
            co_return out;
        }(&sched, &known));

    // its own, and the first 255 entries.
    REQUIRE(out.first.has_value());
    CHECK(out.first->size() == 256);
    CHECK(out.first->back() == "https://h254.test");

    REQUIRE(out.full.has_value());
    CHECK(*out.full == *out.first);

    // room for one more.
    CHECK(out.removed);
    REQUIRE(out.room.has_value());
    CHECK(out.room->size() == 256);
    CHECK(out.room->back() == "https://more.test");
}

TEST_CASE("an HTTP/2 client's connection removes origins from its Origin Set", "[tls][http][client][http2][origin]")
{
    told known;

    net::io::scheduler sched{make_pool()};
    known.sched = &sched;

    struct seen
    {
        bool                                    authoritative = false; // for b.test, before it's removed
        std::vector<bool>                       removed;
        std::optional<std::vector<std::string>> left;
        bool                                    still  = true; // for b.test, after
        std::size_t                             checks = 0;
    };

    const auto out = sched.run_to_completion(
        [](net::io::scheduler* sched, told* known) -> task<seen>
        {
            auto c      = co_await connect(sched, transport::tls, known);
            auto server = played(*c.server);

            seen       out;
            const bool before = c.conn->remove_origin("https://b.test"); // uninitialized: nothing to remove
            const bool sent   = co_await send(server, {"https://b.test", "https://c.test"});
            REQUIRE(sent);

            out.authoritative = c.conn->authoritative_for("https://b.test");
            out.removed       = {before,
                                 c.conn->remove_origin("https://B.test:443"), // normalized as the entries are
                                 c.conn->remove_origin("https://b.test"),     // gone already
                                 c.conn->remove_origin("not an origin"),
                                 c.conn->remove_origin("https://a.test:" + std::to_string(c.port))};
            out.left          = c.conn->origins();

            // the certificate's still valid for it (and not checked again), but the origin's gone.
            out.still  = c.conn->authoritative_for("https://b.test");
            out.checks = c.conn->certificate_checks();

            c.conn->close();
            co_return out;
        }(&sched, &known));

    CHECK(out.authoritative);
    CHECK(out.removed == std::vector<bool>{false, true, false, false, true});
    REQUIRE(out.left.has_value());
    CHECK(*out.left == std::vector<std::string>{"https://c.test"});
    CHECK_FALSE(out.still);
    CHECK(out.checks == 1);
}

TEST_CASE("an HTTP/2 client's connection keeps no Origin Set over cleartext, nor without verifying the server's "
          "certificate",
          "[tls][http][client][http2][origin]")
{
    const auto how = GENERATE(transport::cleartext, transport::insecure_tls);
    CAPTURE(how == transport::cleartext ? "cleartext" : "insecure_skip_verification");

    told known;

    net::io::scheduler sched{make_pool()};
    known.sched = &sched;

    struct seen
    {
        std::optional<std::vector<std::string>> origins;
        bool                                    authoritative = true;
        std::size_t                             checks        = 1;
    };

    const auto out = sched.run_to_completion(
        [](net::io::scheduler* sched, told* known, transport how) -> task<seen>
        {
            auto c      = co_await connect(sched, how, known);
            auto server = played(*c.server);

            const bool sent = co_await send(server, {"https://b.test"});
            REQUIRE(sent);

            seen out{.origins       = c.conn->origins(),
                     .authoritative = c.conn->authoritative_for("https://b.test"),
                     .checks        = c.conn->certificate_checks()};
            c.conn->close();
            co_return out;
        }(&sched, &known, how));

    CHECK_FALSE(out.origins.has_value());
    CHECK_FALSE(out.authoritative);
    CHECK(out.checks == 0);
    CHECK(known.count.load() == 0);
}

TEST_CASE("the HTTP/2 client's pool gives a connection over TLS its own origin, the first of its Origin Set",
          "[tls][http][client][http2][origin]")
{
    net::io::scheduler sched{make_pool()};

    // cppnet's server, listing two of the origins its certificate has.
    http::router r;
    r.GET("/",
          [](const http::server_request& /*req*/, http::response_writer& resp) -> task<void>
          { co_await resp.send(http::status::NO_CONTENT); });

    http::server_config cfg{.tls = server_with(pki().names_chain, pki().names_key)};
    cfg.http2_origins = {"https://b.test", "https://x.w.test"};
    http::server server{&sched, net::listener::make(&sched, "127.0.0.1", "0").value(), std::move(r), cfg};
    REQUIRE(sched.spawn(server.serve()));

    http::client_options options;
    options.tls.trusted_ca_file = pki().ca;
    auto pool                   = std::make_shared<http::detail::connection_pool>(&sched, std::move(options));

    struct seen
    {
        std::optional<std::vector<std::string>> origins{};
        bool                                    authoritative = false;
    };

    const auto port = std::to_string(server.local_port());
    const auto out  = sched.run_to_completion(
        [](http::detail::connection_pool* pool, std::string port) -> task<seen>
        {
            auto leased = co_await pool->acquire("https", "127.0.0.1", port, true);
            REQUIRE(leased.has_value());
            REQUIRE(leased->http2 != nullptr);
            const auto conn = leased->http2;

            // by the time a response has come, the ORIGIN (sent before the server read anything) has.
            http::client_request req{.method = http::request_method::GET,
                                     .uri    = net::url::parse("https://127.0.0.1:" + port + "/").value()};
            auto                 resp = co_await conn->send(req);
            REQUIRE(resp.has_value());
            CHECK(resp->status_code == http::status::NO_CONTENT);
            resp->body.reset();

            co_return seen{.origins = conn->origins(), .authoritative = conn->authoritative_for("https://b.test")};
        }(pool.get(), port));

    REQUIRE(out.origins.has_value());
    CHECK(*out.origins == std::vector<std::string>{"https://127.0.0.1:" + port, "https://b.test", "https://x.w.test"});
    CHECK(out.authoritative);
}
