#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ratio>
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
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "tls/certificate.hpp"
#include "tls/test_pki.hpp"
#include "tls/tls.hpp"
#include "tls/version.hpp"

namespace tls = net::tls;

using namespace std::chrono_literals;
using net::coro::task;
using test::pki;

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// key_of is a session ticket key of fill's bytes.
tls::session_ticket_key key_of(std::uint8_t fill)
{
    tls::session_ticket_key key{};
    key.fill(std::byte{fill});
    return key;
}

// test_clock is a server's clock (see tls::detail::server_hooks), which a test moves on: it must outlive the contexts
// that use it, and their streams.
struct test_clock
{
    std::atomic<std::int64_t> ahead{0}; // in seconds

    void advance(std::chrono::seconds by) { ahead.fetch_add(by.count()); }

    [[nodiscard]] std::chrono::system_clock::time_point now() const
    {
        return std::chrono::system_clock::now() + std::chrono::seconds{ahead.load()};
    }
};

// server_ctx is a server context with localhost's certificate, and the rest of options, with clock as its clock (if
// it's given).
tls::server_context server_ctx(tls::server_options options = {}, const test_clock* clock = nullptr)
{
    options.certificate_chain = pki().localhost_chain;
    options.private_key       = pki().localhost_key;

    tls::detail::server_hooks hooks;
    if (clock != nullptr) hooks.clock = [clock] { return clock->now(); };

    auto ctx = tls::detail::make_server_context(options, std::move(hooks));
    REQUIRE(ctx.has_value());
    return *ctx;
}

// asking is server options that ask for clients' certificates as auth says, trusting cas.
tls::server_options asking(tls::client_auth auth, std::filesystem::path cas = pki().client_ca)
{
    return {.client_auth = auth, .client_ca_file = std::move(cas)};
}

// server_side is what the server's side of a connection saw, once it's done.
struct server_side
{
    net::io::scheduler*        sched;
    std::error_code            err;
    bool                       resumed = false;
    std::string                saw; // the subject of the client's certificate, or "none"
    std::atomic<std::uint64_t> done{0};

    void finish(std::error_code result)
    {
        err = result;
        done.store(1);
        sched->notify_all(done);
    }

    task<> wait()
    {
        const auto deadline = net::io::clock::now() + 5s;
        while (done.load() == 0)
        {
            if (co_await sched->wait_on(done, 0, deadline)) co_return;
        }
    }
};

// leaf_subject is the subject of the first of certs: "none" if there are none.
std::string leaf_subject(const std::vector<tls::certificate>& certs)
{
    return certs.empty() ? "none" : certs.front().subject;
}

// serve accepts a connection, runs the server's handshake, says hello, and closes it.
task<> serve(net::listener* listener, tls::server_context ctx, std::shared_ptr<server_side> side)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
    {
        side->finish(accepted.error());
        co_return;
    }

    auto s = co_await tls::stream::accept(std::move(*accepted), ctx, net::io::clock::now() + 5s);
    if (!s.has_value())
    {
        side->finish(s.error());
        co_return;
    }

    side->resumed = s->resumed();
    side->saw     = leaf_subject(*s->peer_certificates());

    const auto res = co_await net::io::write_all(*s, std::string_view{"hello"});
    co_await s->close();
    side->finish(res.err);
}

// outcome is how a connection went.
struct outcome
{
    // the client's error: connecting, or reading the server's hello (to its end).
    std::error_code client_err;
    std::error_code server_err;

    bool client_resumed = false;
    bool server_resumed = false;

    // the subjects of the certificates each side saw from the other (see leaf_subject()).
    std::string client_saw;
    std::string server_saw;
};

// connect_once connects to listener (as localhost), which serves with server, returning how that went.
task<outcome>
connect_once(net::io::scheduler* sched, net::listener* listener, tls::server_context server, tls::client_context client)
{
    auto    side = std::make_shared<server_side>(sched);
    outcome out;
    if (!sched->spawn(serve(listener, std::move(server), side)))
    {
        out.client_err = std::make_error_code(std::errc::operation_canceled);
        co_return out;
    }

    auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(listener->local_port()));
    if (!conn.has_value())
    {
        out.client_err = conn.error();
    }
    else
    {
        auto s = co_await tls::stream::connect(std::move(*conn), client, "localhost", net::io::clock::now() + 5s);
        if (!s.has_value())
        {
            out.client_err = s.error();
        }
        else
        {
            out.client_resumed = s->resumed();
            out.client_saw     = leaf_subject(*s->peer_certificates());

            // NOTE: reading takes the server's ticket in too, which comes before its hello.
            auto hello = co_await net::io::read_all(*s);
            if (!hello.has_value()) out.client_err = hello.error();
            else if (*hello != "hello") out.client_err = std::make_error_code(std::errc::bad_message);

            co_await s->close();
        }
    }

    co_await side->wait();
    out.server_err     = side->done.load() != 0 ? side->err : std::make_error_code(std::errc::timed_out);
    out.server_resumed = side->resumed;
    out.server_saw     = side->saw;
    co_return out;
}

// listening is a listener on a port of its own.
net::listener listening(net::io::scheduler* sched)
{
    auto listener = net::listener::make(sched, "127.0.0.1", "0");
    REQUIRE(listener.has_value());
    return std::move(*listener);
}

// lifetime is the tests' tickets' lifetime, where the clock matters.
constexpr std::chrono::seconds lifetime = 1h;

// full and resumption are how a connection's handshake went, on both sides (see resumed()).
constexpr std::string_view full       = "full";
constexpr std::string_view resumption = "resumed";

// resumed is how each of outs' handshakes went: full or resumption, or which side alone resumed; failing (with an
// empty list) if any connection failed.
std::vector<std::string_view> resumed(const std::vector<outcome>& outs)
{
    std::vector<std::string_view> out;
    for (const auto& o : outs)
    {
        CAPTURE(o.client_err.message(), o.server_err.message());
        CHECK(!o.client_err);
        CHECK(!o.server_err);
        if (o.client_err || o.server_err) return {};

        if (o.client_resumed != o.server_resumed) out.emplace_back(o.client_resumed ? "client only" : "server only");
        else out.push_back(o.client_resumed ? resumption : full);
    }
    return out;
}

}

TEST_CASE("a client's next connection resumes its session with a ticket", "[tls][resumption]")
{
    net::io::scheduler sched{make_pool()};
    auto               listener = listening(&sched);

    const auto server = server_ctx();
    const auto client = test::trusting_client();

    const auto test = [](net::io::scheduler*        sched,
                         net::listener*             listener,
                         const tls::server_context* server,
                         const tls::client_context* client) -> task<std::vector<outcome>>
    {
        std::vector<outcome> outs;
        outs.reserve(3);
        // NOTE: a resumption gets a ticket too, so the third resumes as well.
        for (int i = 0; i < 3; ++i) outs.push_back(co_await connect_once(sched, listener, *server, *client));
        co_return outs;
    };

    const auto outs = sched.run_to_completion(test(&sched, &listener, &server, &client));
    CHECK(resumed(outs) == std::vector{full, resumption, resumption});

    // the client still sees the server's certificate: the session's.
    for (const auto& o : outs) CHECK(o.client_saw == "CN=localhost");
}

TEST_CASE("sessions aren't resumed without tickets, or over TLS 1.2", "[tls][resumption]")
{
    struct test_case
    {
        std::string_view name;
        bool             tickets            = true;
        tls::version     server_max_version = tls::version::tls13;
        tls::version     client_max_version = tls::version::tls13;
    };

    const auto tc =
        GENERATE(test_case{.name = "session_tickets off", .tickets = false},
                 test_case{.name = "the server allows TLS 1.2 only", .server_max_version = tls::version::tls12},
                 test_case{.name = "the client allows TLS 1.2 only", .client_max_version = tls::version::tls12});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    auto               listener = listening(&sched);

    const auto server = server_ctx({.max_version = tc.server_max_version, .session_tickets = tc.tickets});
    const auto client = test::client_with({}, {}, tc.client_max_version);

    const auto test = [](net::io::scheduler*        sched,
                         net::listener*             listener,
                         const tls::server_context* server,
                         const tls::client_context* client) -> task<std::vector<outcome>>
    {
        std::vector<outcome> outs;
        outs.reserve(3);
        for (int i = 0; i < 3; ++i) outs.push_back(co_await connect_once(sched, listener, *server, *client));
        co_return outs;
    };

    CHECK(resumed(sched.run_to_completion(test(&sched, &listener, &server, &client))) == std::vector{full, full, full});
}

TEST_CASE("a ticket resumes only with a context that has its key", "[tls][resumption][keys]")
{
    struct test_case
    {
        std::string_view                     name;
        std::vector<tls::session_ticket_key> first_keys;
        std::vector<tls::session_ticket_key> second_keys;
        bool                                 resumes = false;
    };

    const auto tc = GENERATE(
        test_case{
            .name        = "random keys, each its own",
            .first_keys  = {},
            .second_keys = {}
    },
        test_case{.name = "the same keys", .first_keys = {key_of(1)}, .second_keys = {key_of(1)}, .resumes = true},
        test_case{.name        = "the first's key second",
                  .first_keys  = {key_of(1)},
                  .second_keys = {key_of(2), key_of(1)},
                  .resumes     = true},
        test_case{.name = "other keys", .first_keys = {key_of(1)}, .second_keys = {key_of(2)}});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    auto               listener = listening(&sched);

    const auto first  = server_ctx({.session_ticket_keys = tc.first_keys});
    const auto second = server_ctx({.session_ticket_keys = tc.second_keys});
    const auto client = test::trusting_client();

    const auto test = [](net::io::scheduler*        sched,
                         net::listener*             listener,
                         const tls::server_context* first,
                         const tls::server_context* second,
                         const tls::client_context* client) -> task<std::vector<outcome>>
    {
        std::vector<outcome> outs;
        outs.push_back(co_await connect_once(sched, listener, *first, *client));
        outs.push_back(co_await connect_once(sched, listener, *second, *client));
        co_return outs;
    };

    // a ticket that can't be decrypted is a full handshake, not a failure.
    CHECK(resumed(sched.run_to_completion(test(&sched, &listener, &first, &second, &client)))
          == std::vector{full, tc.resumes ? resumption : full});
}

TEST_CASE("set_session_ticket_keys replaces a context's keys while it's used", "[tls][resumption][keys]")
{
    net::io::scheduler sched{make_pool()};
    auto               listener = listening(&sched);

    auto       server = server_ctx({.session_ticket_keys = {key_of(1)}});
    const auto client = test::trusting_client();

    const auto test = [](net::io::scheduler*        sched,
                         net::listener*             listener,
                         tls::server_context*       server,
                         const tls::client_context* client) -> task<std::vector<outcome>>
    {
        std::vector<outcome> outs;

        // a ticket made with key 1, which key 3 doesn't decrypt.
        outs.push_back(co_await connect_once(sched, listener, *server, *client));
        server->set_session_ticket_keys({key_of(3)});
        outs.push_back(co_await connect_once(sched, listener, *server, *client));

        // its ticket (made with key 3) resumes with key 3 kept second.
        server->set_session_ticket_keys({key_of(2), key_of(3)});
        outs.push_back(co_await connect_once(sched, listener, *server, *client));

        // none: no tickets made or resumed.
        server->set_session_ticket_keys({});
        outs.push_back(co_await connect_once(sched, listener, *server, *client));
        outs.push_back(co_await connect_once(sched, listener, *server, *client));

        // and keys again: tickets again.
        server->set_session_ticket_keys({key_of(4)});
        outs.push_back(co_await connect_once(sched, listener, *server, *client));
        outs.push_back(co_await connect_once(sched, listener, *server, *client));
        co_return outs;
    };

    CHECK(resumed(sched.run_to_completion(test(&sched, &listener, &server, &client)))
          == std::vector{full, full, resumption, full, full, full, resumption});
}

TEST_CASE("a ticket resumes for its lifetime, and random keys are replaced each lifetime",
          "[tls][resumption][lifetime]")
{
    test_clock         clock;
    net::io::scheduler sched{make_pool()};
    auto               listener = listening(&sched);

    const bool given_keys = GENERATE(false, true);
    CAPTURE(given_keys);

    tls::server_options options{.session_ticket_lifetime = lifetime};
    if (given_keys) options.session_ticket_keys = {key_of(1)};
    const auto server = server_ctx(options, &clock);
    const auto client = test::trusting_client();

    const auto test = [](net::io::scheduler*        sched,
                         net::listener*             listener,
                         test_clock*                clock,
                         const tls::server_context* server,
                         const tls::client_context* client) -> task<std::vector<outcome>>
    {
        std::vector<outcome> outs;

        // a ticket made now (with the first random key).
        outs.push_back(co_await connect_once(sched, listener, *server, *client));

        // three quarters of a lifetime on: the ticket resumes (and another is made, with the same key).
        clock->advance(lifetime * 3 / 4);
        outs.push_back(co_await connect_once(sched, listener, *server, *client));

        // a lifetime and a half on: the first random key is replaced, and kept to resume the last ticket (three
        // quarters of a lifetime old) with.
        clock->advance(lifetime * 3 / 4);
        outs.push_back(co_await connect_once(sched, listener, *server, *client));

        // a lifetime and a second later, the last ticket has expired (a full handshake, and a ticket for it).
        clock->advance(lifetime + 1s);
        outs.push_back(co_await connect_once(sched, listener, *server, *client));

        // and two lifetimes on, so has that one, and its key.
        clock->advance(lifetime * 2);
        outs.push_back(co_await connect_once(sched, listener, *server, *client));
        outs.push_back(co_await connect_once(sched, listener, *server, *client));
        co_return outs;
    };

    CHECK(resumed(sched.run_to_completion(test(&sched, &listener, &clock, &server, &client)))
          == std::vector{full, resumption, resumption, full, full, resumption});
}

TEST_CASE("tickets are bound to the client authentication", "[tls][resumption][client_auth]")
{
    struct test_case
    {
        std::string_view    name;
        tls::server_options first;
        tls::server_options second;
        bool                resumes = false;
    };

    const auto required = tls::client_auth::required;
    const auto optional = tls::client_auth::optional;

    const auto tc = GENERATE_COPY(
        test_case{.name = "the same", .first = asking(required), .second = asking(required), .resumes = true},
        test_case{.name = "optional", .first = asking(optional), .second = asking(optional), .resumes = true},
        test_case{.name = "another mode", .first = asking(required), .second = asking(optional)},
        test_case{.name   = "other client CAs",
                  .first  = asking(required),
                  .second = asking(required, pki().client_ca_and_unnamed)});
    CAPTURE(tc.name);

    net::io::scheduler sched{make_pool()};
    auto               listener = listening(&sched);

    // NOTE: with the same keys, so only what the tickets are bound to differs.
    auto first                 = tc.first;
    auto second                = tc.second;
    first.session_ticket_keys  = {key_of(1)};
    second.session_ticket_keys = {key_of(1)};
    const auto server1         = server_ctx(first);
    const auto server2         = server_ctx(second);
    const auto client          = test::client_with(pki().client_chain, pki().client_key);

    const auto test = [](net::io::scheduler*        sched,
                         net::listener*             listener,
                         const tls::server_context* first,
                         const tls::server_context* second,
                         const tls::client_context* client) -> task<std::vector<outcome>>
    {
        std::vector<outcome> outs;
        outs.push_back(co_await connect_once(sched, listener, *first, *client));
        outs.push_back(co_await connect_once(sched, listener, *second, *client));
        co_return outs;
    };

    const auto outs = sched.run_to_completion(test(&sched, &listener, &server1, &server2, &client));
    CHECK(resumed(outs) == std::vector{full, tc.resumes ? resumption : full});

    // the server sees the client's certificate either way: verified anew, or the session's.
    for (const auto& o : outs) CHECK(o.server_saw == pki().client_subject);
}

TEST_CASE("a session whose client certificate has expired since isn't resumed", "[tls][resumption][client_auth]")
{
    test_clock         clock;
    net::io::scheduler sched{make_pool()};
    auto               listener = listening(&sched);

    // NOTE: the client's certificate expires in a day; its tickets are good for two.
    auto options                    = asking(GENERATE(tls::client_auth::required, tls::client_auth::optional));
    options.session_ticket_lifetime = 48h;
    const auto server               = server_ctx(options, &clock);
    const auto client               = test::client_with(pki().client_chain, pki().client_key);

    const auto test = [](net::io::scheduler*        sched,
                         net::listener*             listener,
                         test_clock*                clock,
                         const tls::server_context* server,
                         const tls::client_context* client) -> task<std::vector<outcome>>
    {
        std::vector<outcome> outs;
        outs.push_back(co_await connect_once(sched, listener, *server, *client));

        // the client's certificate has expired: so the session isn't resumed, and the certificate fails verification.
        clock->advance(25h);
        outs.push_back(co_await connect_once(sched, listener, *server, *client));
        co_return outs;
    };

    const auto outs = sched.run_to_completion(test(&sched, &listener, &clock, &server, &client));
    REQUIRE(outs.size() == 2);

    CHECK(!outs[0].client_err);
    CHECK(!outs[0].server_err);
    CHECK(outs[0].server_saw == pki().client_subject);

    // over TLS 1.3, the client's handshake is done before the server looks at its certificate: its read fails.
    CHECK(!outs[1].client_resumed);
    CHECK(outs[1].client_err == tls::alert::bad_certificate);
    CHECK(tls::from_peer(outs[1].client_err));
    CHECK(outs[1].server_err.category() == tls::verification_category());
    CHECK(outs[1].server_err.message().contains("expired"));
}

TEST_CASE("a client offers a ticket only to the host and port it came from", "[tls][resumption][client]")
{
    net::io::scheduler sched{make_pool()};
    auto               listener1 = listening(&sched);
    auto               listener2 = listening(&sched);

    // NOTE: servers with the same keys, so either would resume the other's tickets.
    const auto server = server_ctx({.session_ticket_keys = {key_of(1)}});
    const auto client = test::trusting_client();

    const auto test = [](net::io::scheduler*        sched,
                         net::listener*             listener1,
                         net::listener*             listener2,
                         const tls::server_context* server,
                         const tls::client_context* client) -> task<std::vector<outcome>>
    {
        std::vector<outcome> outs;
        outs.push_back(co_await connect_once(sched, listener1, *server, *client));
        outs.push_back(co_await connect_once(sched, listener2, *server, *client));
        outs.push_back(co_await connect_once(sched, listener1, *server, *client));
        outs.push_back(co_await connect_once(sched, listener2, *server, *client));
        co_return outs;
    };

    // the first port's ticket isn't offered to the second (or it would be used up): each port's resumes its own.
    CHECK(resumed(sched.run_to_completion(test(&sched, &listener1, &listener2, &server, &client)))
          == std::vector{full, full, resumption, resumption});
}

TEST_CASE("a client offers each ticket once", "[tls][resumption][client]")
{
    net::io::scheduler sched{make_pool()};
    auto               listener = listening(&sched);

    auto       server = server_ctx({.session_ticket_keys = {key_of(1)}});
    const auto client = test::trusting_client();

    const auto test = [](net::io::scheduler*        sched,
                         net::listener*             listener,
                         tls::server_context*       server,
                         const tls::client_context* client) -> task<std::vector<outcome>>
    {
        std::vector<outcome> outs;

        // a ticket, offered while the server has no keys (so no ticket is made either).
        outs.push_back(co_await connect_once(sched, listener, *server, *client));
        server->set_session_ticket_keys({});
        outs.push_back(co_await connect_once(sched, listener, *server, *client));

        // with its key back, the ticket would resume: but it's used up.
        server->set_session_ticket_keys({key_of(1)});
        outs.push_back(co_await connect_once(sched, listener, *server, *client));
        co_return outs;
    };

    CHECK(resumed(sched.run_to_completion(test(&sched, &listener, &server, &client))) == std::vector{full, full, full});
}

TEST_CASE("a server context's session ticket options are checked", "[tls][resumption][options]")
{
    struct test_case
    {
        std::string_view    name;
        tls::server_options options;
        bool                valid = false;
    };

    const auto tc = GENERATE_COPY(
        test_case{
            .name    = "defaults",
            .options = {},
            .valid   = true
    },
        test_case{.name    = "a lifetime of 7 days",
                  .options = {.session_ticket_lifetime = std::chrono::days{7}},
                  .valid   = true},
        test_case{.name = "a lifetime over 7 days", .options = {.session_ticket_lifetime = std::chrono::days{7} + 1s}},
        test_case{.name = "a lifetime of 0", .options = {.session_ticket_lifetime = 0s}},
        test_case{.name = "a negative lifetime", .options = {.session_ticket_lifetime = -1s}},
        test_case{.name = "keys", .options = {.session_ticket_keys = {key_of(1), key_of(2)}}, .valid = true},
        test_case{.name = "no tickets", .options = {.session_tickets = false}, .valid = true},
        test_case{.name    = "keys without tickets",
                  .options = {.session_tickets = false, .session_ticket_keys = {key_of(1)}}});
    CAPTURE(tc.name);

    auto options              = tc.options;
    options.certificate_chain = pki().localhost_chain;
    options.private_key       = pki().localhost_key;

    const auto ctx = tls::server_context::make(options);
    if (tc.valid)
    {
        CHECK(ctx.has_value());
    }
    else
    {
        REQUIRE(!ctx.has_value());
        CHECK(ctx.error() == std::errc::invalid_argument);
    }
}

TEST_CASE("set_session_ticket_keys does nothing without session tickets", "[tls][resumption][keys]")
{
    net::io::scheduler sched{make_pool()};
    auto               listener = listening(&sched);

    auto server = server_ctx({.session_tickets = false});
    server.set_session_ticket_keys({key_of(1)});
    const auto client = test::trusting_client();

    const auto test = [](net::io::scheduler*        sched,
                         net::listener*             listener,
                         const tls::server_context* server,
                         const tls::client_context* client) -> task<std::vector<outcome>>
    {
        std::vector<outcome> outs;
        outs.push_back(co_await connect_once(sched, listener, *server, *client));
        outs.push_back(co_await connect_once(sched, listener, *server, *client));
        co_return outs;
    };

    CHECK(resumed(sched.run_to_completion(test(&sched, &listener, &server, &client))) == std::vector{full, full});
}
