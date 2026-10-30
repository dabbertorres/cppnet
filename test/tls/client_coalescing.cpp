// The HTTP/2 client's connection coalescing (RFC 8336): with client_options::http2_coalescing, a request whose host has
// no connection free takes a stream of another host's whose server listed the request's origin in ORIGIN, and whose
// certificate is valid for its host; and a 421 to a request sent so has it sent again on a connection of its own
// host's. The servers are on loopback, with a certificate for a.test, b.test, *.w.test and 127.0.0.1; a resolver whose
// hosts table names a.test, b.test and x.w.test 127.0.0.1 (and nothing else) finds them. Each host has a server of its
// own, on a port of its own, so which server a request reached says which connection it went on: b.test's own server,
// or another host's whose ORIGIN listed b.test's origin (its port being b.test's server's).
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
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
#include "dns/config.hpp"
#include "dns/error.hpp"
#include "dns/resolver.hpp"
#include "http/client.hpp"
#include "http/connection.hpp"
#include "http/connection_pool.hpp"
#include "http/h2_client.hpp"
#include "http/http2_client.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "ip_addr.hpp"
#include "listen.hpp"
#include "tls/test_pki.hpp"
#include "tls/tls.hpp"
#include "url.hpp"
#include "websocket/websocket.hpp"

namespace http  = net::http;
namespace http2 = net::http::http2;
namespace dns   = net::dns;
namespace tls   = net::tls;

using namespace std::chrono_literals;
using net::coro::task;
using test::pki;
using test::server_with;

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// names is a resolver that knows a.test, b.test and x.w.test, as 127.0.0.1, from its hosts table alone.
dns::resolver::options names()
{
    const auto loopback = net::ip_addr::parse("127.0.0.1").value();

    dns::config conf;
    conf.hosts = {
        {  "a.test", {loopback}},
        {  "b.test", {loopback}},
        {"x.w.test", {loopback}},
    };
    conf.order = dns::lookup_order::files;
    return {.use = dns::resolver::backend::native, .conf = std::move(conf)};
}

// served is what a server's handler has seen, and how it answers b.test's requests.
struct served
{
    net::io::scheduler* sched = nullptr;

    // how many of b.test's requests (from the first) it answers with 421; and how many of them it holds, each until
    // they've all come (none: it holds none); and whether it holds them until the test opens the gate.
    std::uint64_t misdirect = 0;
    std::uint64_t hold      = 0;
    bool          gated     = false;

    std::atomic<std::uint64_t> b_arrived{0};
    std::atomic<std::uint64_t> gate{0};

    std::mutex               mu;
    std::vector<std::string> hosts;    // each request's, in order (guarded by mu)
    std::vector<std::string> b_bodies; // b.test's requests' bodies (guarded by mu)

    // count is how many requests it's had for host.
    std::size_t count(std::string_view host)
    {
        std::scoped_lock lock{mu};
        return static_cast<std::size_t>(std::ranges::count(hosts, host));
    }

    std::vector<std::string> bodies()
    {
        std::scoped_lock lock{mu};
        return b_bodies;
    }

    // reached waits (up to 10 seconds) until word is at least n.
    task<> reached(std::atomic<std::uint64_t>& word, std::uint64_t n) const
    {
        const auto deadline = net::io::clock::now() + 10s;
        for (auto seen = word.load(); seen < n; seen = word.load())
        {
            if (co_await sched->wait_on(word, seen, deadline)) co_return;
        }
    }

    // open opens the gate.
    void open()
    {
        gate.store(1);
        sched->notify_all(gate);
    }
};

// answer answers a request with its host, and its body after a space, if it has one; or b.test's with 421, as state
// says.
http::handler_func answer(served* state)
{
    return [state](http::server_request& req, http::response_writer& resp) -> task<void>
    {
        auto        body = co_await net::io::read_all(*req.body);
        std::string text = req.host;
        if (body.has_value() && !body->empty()) text += " " + *body;

        {
            std::scoped_lock lock{state->mu};
            state->hosts.push_back(req.host);
            if (req.host == "b.test") state->b_bodies.push_back(body.value_or("(failed)"));
        }

        if (req.host == "b.test")
        {
            const auto n = state->b_arrived.fetch_add(1);
            state->sched->notify_all(state->b_arrived);
            if (state->hold > 0) co_await state->reached(state->b_arrived, state->hold);
            if (state->gated) co_await state->reached(state->gate, 1);

            if (n < state->misdirect)
            {
                (void)co_await resp.send(http::status::MISDIRECTED_REQUEST, 0);
                co_return;
            }
        }

        auto* out = co_await resp.send(http::status::OK, text.size());
        co_await net::io::write_all(*out, text);
    };
}

// listen is a listener on loopback, on a port the OS chooses.
net::listener listen(net::io::scheduler* sched) { return net::listener::make(sched, "127.0.0.1", "0").value(); }

// origin is host's https origin, on port.
std::string origin(std::string_view host, std::uint16_t port) { return std::format("https://{}:{}", host, port); }

// how is how a server serves.
struct how
{
    bool          cleartext   = false; // rather than over TLS
    bool          http2       = true;  // or only HTTP/1.1
    std::uint32_t max_streams = 100;   // of an HTTP/2 connection's, at once
};

// serve is cppnet's server on listener, over TLS with the certificate for a.test, b.test and *.w.test (unless
// cleartext), listing origins in ORIGIN (none sent over cleartext), answering as state says.
std::unique_ptr<http::server>
serve(net::io::scheduler* sched, net::listener listener, served* state, std::vector<std::string> origins, how as = {})
{
    state->sched = sched;

    http::router r;
    r.GET("/", answer(state));
    r.POST("/", answer(state));

    // a WebSocket, recorded as "ws", its host and its HTTP version, read until it ends.
    r.GET("/ws",
          [state](http::server_request& req, http::response_writer& rw) -> task<void>
          {
              auto conn = co_await net::websocket::accept(req, rw);
              if (!conn.has_value()) co_return;
              {
                  std::scoped_lock lock{state->mu};
                  state->hosts.push_back(std::format("ws {} HTTP/{}", req.host, req.version.major));
              }

              std::string message;
              while ((co_await conn->read_message(message)).has_value())
              {}
          });

    http::server_config cfg;
    if (!as.cleartext) cfg.tls = server_with(pki().names_chain, pki().names_key);
    cfg.http2                        = as.http2;
    cfg.http2_max_concurrent_streams = as.max_streams;
    cfg.http2_origins                = std::move(origins);

    auto server = std::make_unique<http::server>(sched, std::move(listener), std::move(r), cfg);
    REQUIRE(sched->spawn(server->serve()));
    return server;
}

// options_for is a client's options, trusting the certificate's CA, with resolver.
http::client_options options_for(dns::resolver* resolver, bool coalescing)
{
    http::client_options options;
    options.resolver            = resolver;
    options.tls.trusted_ca_file = pki().ca;
    options.http2_coalescing    = coalescing;
    return options;
}

// fetched is a response's status and body.
struct fetched
{
    http::status status = http::status::NONE;
    std::string  body;
};

// request is what fetch() sends.
struct request
{
    std::optional<std::string> body       = {};    // a POST's, sent with set_body(); none: a GET
    bool                       rewindable = true;  // the body can be read again (set_body()'s get_body)
    bool                       close      = false; // with "Connection: close" (meaningless over HTTP/2)
};

// fetch sends a request for url, and reads the response.
task<std::expected<fetched, std::error_code>> fetch(http::client& c, std::string url, request r = {})
{
    http::client_request req{.method = http::request_method::GET, .uri = net::url::parse(url).value()};
    if (r.body.has_value())
    {
        req.method = http::request_method::POST;
        req.set_body(std::move(*r.body));
        if (!r.rewindable) req.get_body = nullptr;
    }
    if (r.close) req.headers.set("Connection", "close");

    auto resp = co_await c.send(std::move(req));
    if (!resp.has_value()) co_return std::unexpected(resp.error());

    auto text = co_await net::io::read_all(*resp->body);
    if (!text.has_value()) co_return std::unexpected(text.error());
    co_return fetched{.status = resp->status_code, .body = std::move(*text)};
}

// url_of is the URL of / on host, at port.
std::string url_of(std::string_view host, std::uint16_t port, bool cleartext = false)
{
    return std::format("{}://{}:{}/", cleartext ? "http" : "https", host, port);
}

// warm_up sends two requests to host, so its connection's Origin Set (from the ORIGIN that came before the first
// response) has been told of to the pool by the time the second's response has come: the first's might have come in
// the same read, and so before the connection's told of the set; the second's comes in a read after.
task<bool> warm_up(http::client& c, std::string_view host, std::uint16_t port, bool cleartext = false)
{
    for (int i = 0; i < 2; ++i)
    {
        const auto got = co_await fetch(c, url_of(host, port, cleartext));
        if (!got.has_value() || got->status != http::status::OK) co_return false;
    }
    co_return true;
}

// sent is what a test's requests to b.test came to, and the client's connections once they had (read before the
// scheduler stops, which ends them).
struct sent
{
    bool                                                 warmed = false;
    std::vector<std::expected<fetched, std::error_code>> got;
    std::size_t                                          open   = 0; // altogether
    std::size_t                                          b_open = 0; // b.test's
};

// scheme is which of a.test and b.test is reached over cleartext (http, rather than https).
struct scheme
{
    bool a_cleartext = false;
    bool b_cleartext = false;
};

// to_b warms up a.test's connection (at a_port), then sends requests to b.test (at b_port), one after another.
task<sent>
to_b(http::client* c, std::uint16_t a_port, std::uint16_t b_port, std::vector<request> requests, scheme over = {})
{
    const bool warmed = co_await warm_up(*c, "a.test", a_port, over.a_cleartext);
    sent       out{.warmed = warmed, .got = {}, .open = 0, .b_open = 0};
    for (auto& r : requests)
        out.got.push_back(co_await fetch(*c, url_of("b.test", b_port, over.b_cleartext), std::move(r)));
    out.open   = c->stats().open;
    out.b_open = c->stats("b.test", std::to_string(b_port)).open;
    co_return out;
}

// ok is whether a request came to a 200 with body.
bool ok(const std::expected<fetched, std::error_code>& got, std::string_view body = "b.test")
{
    return got.has_value() && got->status == http::status::OK && got->body == body;
}

// status_of is a request's response's status, if it had one.
http::status status_of(const std::expected<fetched, std::error_code>& got)
{
    return got.has_value() ? got->status : http::status::NONE;
}

}

TEST_CASE("with coalescing on, a request goes on another host's HTTP/2 connection whose server listed its origin",
          "[tls][http][client][http2][coalescing]")
{
    dns::resolver resolver{names()};
    served        a_seen;
    served        b_seen;

    net::io::scheduler sched{make_pool()};
    auto               la       = listen(&sched);
    auto               lb       = listen(&sched);
    const auto         pa       = la.local_port();
    const auto         pb       = lb.local_port();
    const auto         a_server = serve(&sched, std::move(la), &a_seen, {origin("a.test", pa), origin("b.test", pb)});
    const auto         b_server = serve(&sched, std::move(lb), &b_seen, {});

    http::client client{&sched, options_for(&resolver, true)};

    const auto out = sched.run_to_completion(to_b(&client, pa, pb, {request{}}));

    REQUIRE(out.warmed);
    REQUIRE(out.got.size() == 1);
    CHECK(ok(out.got[0]));

    // on a.test's connection, so a.test's server had it; and no connection of b.test's.
    CHECK(a_seen.count("b.test") == 1);
    CHECK(b_seen.count("b.test") == 0);
    CHECK(out.open == 1);
    CHECK(out.b_open == 0);
}

TEST_CASE("a WebSocket goes on its own host's HTTP/2 connection, never another host's that would coalesce a request",
          "[tls][http][client][http2][coalescing][websocket]")
{
    dns::resolver resolver{names()};
    served        a_seen;
    served        b_seen;

    net::io::scheduler sched{make_pool()};
    auto               la       = listen(&sched);
    auto               lb       = listen(&sched);
    const auto         pa       = la.local_port();
    const auto         pb       = lb.local_port();
    const auto         a_server = serve(&sched, std::move(la), &a_seen, {origin("a.test", pa), origin("b.test", pb)});
    const auto         b_server = serve(&sched, std::move(lb), &b_seen, {});

    http::client client{&sched, options_for(&resolver, true)};

    // a.test's connection lists b.test's origin, so a request to b.test would go on it; a WebSocket to b.test goes over
    // HTTP/1.1 to b.test's own server instead (it has no HTTP/2 connection of its own), and one to a.test on a.test's.
    const auto test = [&] -> task<std::vector<std::string>>
    {
        std::vector<std::string> lines;
        lines.emplace_back(co_await warm_up(client, "a.test", pa) ? "warmed" : "not warmed");

        for (const auto& url : {std::format("wss://b.test:{}/ws", pb), std::format("wss://a.test:{}/ws", pa)})
        {
            auto conn = co_await net::websocket::connect(client, url);
            if (!conn.has_value())
            {
                lines.push_back("connect: " + conn.error().message());
                continue;
            }
            lines.emplace_back(co_await conn->close() ? "close failed" : "closed");
        }

        lines.push_back(std::format("open={}", client.stats().open));
        co_return lines;
    };

    CHECK(sched.run_to_completion(test()) == std::vector<std::string>{"warmed", "closed", "closed", "open=1"});
    CHECK(b_seen.count("ws b.test HTTP/1") == 1);
    CHECK(a_seen.count("ws a.test HTTP/2") == 1);
    CHECK(a_seen.count("ws b.test HTTP/2") == 0);
}

TEST_CASE("a request isn't coalesced without ORIGIN, with coalescing off, for an origin not listed, without verifying "
          "certificates, over cleartext, nor for an http URL",
          "[tls][http][client][http2][coalescing]")
{
    enum class kind : std::uint8_t
    {
        no_origin,
        off,
        not_listed,
        insecure,
        cleartext,
        http_url, // to b.test over cleartext, while a.test's connection over TLS lists its port's https origin
    };
    const auto k =
        GENERATE(kind::no_origin, kind::off, kind::not_listed, kind::insecure, kind::cleartext, kind::http_url);
    CAPTURE(static_cast<int>(k));

    dns::resolver resolver{names()};
    served        a_seen;
    served        b_seen;

    net::io::scheduler sched{make_pool()};
    auto               la = listen(&sched);
    auto               lb = listen(&sched);
    const auto         pa = la.local_port();
    const auto         pb = lb.local_port();

    // not listed: b.test on a.test's server's port, which isn't the request's origin.
    std::vector<std::string> origins{origin("a.test", pa), origin("b.test", pb)};
    if (k == kind::no_origin) origins.clear();
    if (k == kind::not_listed) origins = {origin("a.test", pa), origin("b.test", pa)};

    const scheme over{.a_cleartext = k == kind::cleartext, .b_cleartext = k == kind::cleartext || k == kind::http_url};
    const auto   a_server = serve(&sched, std::move(la), &a_seen, origins, {.cleartext = over.a_cleartext});
    const auto   b_server = serve(&sched, std::move(lb), &b_seen, {}, {.cleartext = over.b_cleartext});

    auto options = options_for(&resolver, k != kind::off);
    if (k == kind::insecure) options.tls.insecure_skip_verification = true;
    if (over.b_cleartext) options.http2_prior_knowledge = true;
    http::client client{&sched, options};

    const auto out = sched.run_to_completion(to_b(&client, pa, pb, {request{}}, over));

    REQUIRE(out.warmed);
    REQUIRE(out.got.size() == 1);
    CHECK(ok(out.got[0]));

    // b.test's own server had it, on a connection of its own.
    CHECK(a_seen.count("b.test") == 0);
    CHECK(b_seen.count("b.test") == 1);
    CHECK(out.open == 2);
    CHECK(out.b_open == 1);
}

TEST_CASE("a request whose host has an idle connection takes it, rather than being coalesced",
          "[tls][http][client][http2][coalescing]")
{
    dns::resolver resolver{names()};
    served        a_seen;
    served        b_seen;

    net::io::scheduler sched{make_pool()};
    auto               la       = listen(&sched);
    auto               lb       = listen(&sched);
    const auto         pa       = la.local_port();
    const auto         pb       = lb.local_port();
    const auto         a_server = serve(&sched, std::move(la), &a_seen, {origin("a.test", pa), origin("b.test", pb)});

    // HTTP/1.1, so its connection's idle between requests (and not shared).
    const auto b_server = serve(&sched, std::move(lb), &b_seen, {}, {.http2 = false});

    http::client client{&sched, options_for(&resolver, true)};

    struct outcome
    {
        std::expected<fetched, std::error_code> first;
        sent                                    then;
    };
    const auto out = sched.run_to_completion(
        [](http::client* c, std::uint16_t pa, std::uint16_t pb) -> task<outcome>
        {
            // b.test first, so it has an idle connection when a.test's server lists it.
            auto first = co_await fetch(*c, url_of("b.test", pb));
            co_return outcome{.first = std::move(first), .then = co_await to_b(c, pa, pb, {request{}})};
        }(&client, pa, pb));

    CHECK(ok(out.first));
    REQUIRE(out.then.warmed);
    REQUIRE(out.then.got.size() == 1);
    CHECK(ok(out.then.got[0]));

    // both on b.test's connection.
    CHECK(a_seen.count("b.test") == 0);
    CHECK(b_seen.count("b.test") == 2);
    CHECK(out.then.b_open == 1);
}

namespace
{

// played_server is a server the test plays, over TLS with the certificate for a.test and b.test: it accepts one
// connection on its listener (which run() owns), and runs the handshake, as a task of its own while the client's pool
// connects. NOTE: declared before the scheduler, as its task uses it.
struct played_server
{
    net::io::scheduler*                                        sched = nullptr;
    std::optional<std::expected<tls::stream, std::error_code>> result;
    std::atomic<std::uint64_t>                                 done{0};

    static task<> run(played_server* self, net::listener listener)
    {
        auto accepted = co_await listener.accept();
        if (accepted.has_value())
        {
            self->result = co_await tls::stream::accept(std::move(*accepted),
                                                        server_with(pki().names_chain, pki().names_key),
                                                        net::io::clock::now() + 5s,
                                                        {.alpn = std::vector<std::string>{"h2"}});
        }

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

}

TEST_CASE("a request isn't coalesced onto a connection whose server listed its origin, if the certificate isn't valid "
          "for its host",
          "[tls][http][client][http2][coalescing]")
{
    dns::resolver resolver{names()};

    played_server server;

    net::io::scheduler sched{make_pool()};
    server.sched       = &sched;
    auto       l       = listen(&sched);
    const auto port    = std::to_string(l.local_port());
    const bool spawned = sched.spawn(played_server::run(&server, std::move(l)));
    REQUIRE(spawned);
    auto pool = std::make_shared<http::detail::connection_pool>(&sched, options_for(&resolver, true));

    struct outcome
    {
        bool                           same      = false; // b.test's lease is a.test's connection
        bool                           coalesced = false; // and says so
        std::string                    key;
        std::optional<std::error_code> c_error; // c.test's acquire's error, if it failed
        bool                           c_coalesced = false;
    };

    const auto out = sched.run_to_completion(
        [](http::detail::connection_pool* pool, played_server* server, std::string port) -> task<outcome>
        {
            auto a = co_await pool->acquire("https", "a.test", port, true);
            REQUIRE(a.has_value());
            REQUIRE(a->http2 != nullptr);
            co_await server->wait();
            REQUIRE(server->result.has_value());
            REQUIRE(server->result->has_value());

            http::detail::connection side{std::move(**server->result)};
            test::h2_client          played{&http::detail::as_reader(side),
                                            &http::detail::as_writer(side),
                                            &http::detail::socket_of(side),
                                            http2::role::server};

            // c.test is listed, but the certificate doesn't have it (cppnet's server would refuse to list it). The
            // PINGs make sure the client's done with the receive the ORIGIN came in (see client_origin.cpp).
            const std::vector<std::string> origins{"https://b.test:" + port, "https://c.test:" + port};
            REQUIRE(!played.s.submit_origin(origins));
            const bool synced = co_await played.sync();
            REQUIRE(synced);
            const bool synced_again = co_await played.sync();
            REQUIRE(synced_again);

            outcome out;

            // b.test's is coalesced.
            auto b = co_await pool->acquire("https", "b.test", port, true);
            REQUIRE(b.has_value());
            out.same      = b->http2 == a->http2;
            out.coalesced = b->coalesced;
            out.key       = b->key;

            // c.test's isn't: it's looked up (and isn't found), rather than taking a.test's connection.
            auto c = co_await pool->acquire("https", "c.test", port, true);
            if (c.has_value()) out.c_coalesced = c->coalesced;
            else out.c_error = c.error();

            a->http2->close();
            co_return out;
        }(pool.get(), &server, port));

    CHECK(out.same);
    CHECK(out.coalesced);
    CHECK(out.key == "https://a.test " + port);
    CHECK_FALSE(out.c_coalesced);
    REQUIRE(out.c_error.has_value());
    CHECK(*out.c_error == dns::condition::not_found);
}

TEST_CASE("a coalesced request's 421 sends it again on a connection of its own host's, and removes its origin from "
          "the connection's Origin Set",
          "[tls][http][client][http2][coalescing]")
{
    // b.test's server also answers 421 (to the request sent again): that's returned.
    const bool own_misdirects = GENERATE(false, true);
    CAPTURE(own_misdirects);

    dns::resolver resolver{names()};
    served        a_seen;
    served        b_seen;
    a_seen.misdirect = 1;
    b_seen.misdirect = own_misdirects ? 1 : 0;

    net::io::scheduler sched{make_pool()};
    auto               la       = listen(&sched);
    auto               lb       = listen(&sched);
    const auto         pa       = la.local_port();
    const auto         pb       = lb.local_port();
    const auto         a_server = serve(&sched, std::move(la), &a_seen, {origin("a.test", pa), origin("b.test", pb)});

    // HTTP/1.1, so a request with "Connection: close" leaves b.test no connection: the second has to look again, and
    // a.test's connection, without b.test's origin, doesn't take it. Or, when it answers 421 itself, HTTP/2, as only an
    // HTTP/2 connection's 421 could be taken for a coalesced one's (the second then goes on its connection).
    const auto b_server = serve(&sched, std::move(lb), &b_seen, {}, {.http2 = own_misdirects});

    http::client client{&sched, options_for(&resolver, true)};

    const auto out = sched.run_to_completion(to_b(&client, pa, pb, {{.close = true}, {.close = true}}));

    REQUIRE(out.warmed);
    REQUIRE(out.got.size() == 2);
    if (own_misdirects) CHECK(status_of(out.got[0]) == http::status::MISDIRECTED_REQUEST);
    else CHECK(ok(out.got[0]));
    CHECK(ok(out.got[1]));

    // the first went to a.test's server (421), then b.test's (and not again); the second to b.test's.
    CHECK(a_seen.count("b.test") == 1);
    CHECK(b_seen.count("b.test") == 2);
}

TEST_CASE("a coalesced request with a body is sent again after a 421 only if it can be read again",
          "[tls][http][client][http2][coalescing]")
{
    const bool rewindable = GENERATE(true, false);
    CAPTURE(rewindable);

    dns::resolver resolver{names()};
    served        a_seen;
    served        b_seen;
    a_seen.misdirect = 1;

    net::io::scheduler sched{make_pool()};
    auto               la       = listen(&sched);
    auto               lb       = listen(&sched);
    const auto         pa       = la.local_port();
    const auto         pb       = lb.local_port();
    const auto         a_server = serve(&sched, std::move(la), &a_seen, {origin("a.test", pa), origin("b.test", pb)});
    const auto         b_server = serve(&sched, std::move(lb), &b_seen, {}, {.http2 = false});

    http::client client{&sched, options_for(&resolver, true)};

    // a POST, then a GET, which shows whether a.test's connection still takes b.test's requests.
    const auto out = sched.run_to_completion(to_b(&client,
                                                  pa,
                                                  pb,
                                                  {
                                                      {.body = "posted", .rewindable = rewindable, .close = true},
                                                      {.close = true}
    }));

    REQUIRE(out.warmed);
    REQUIRE(out.got.size() == 2);
    CHECK(a_seen.bodies() == std::vector<std::string>{"posted"});

    if (rewindable)
    {
        // sent again, with its body.
        CHECK(ok(out.got[0], "b.test posted"));
        CHECK(b_seen.bodies() == std::vector<std::string>{"posted", ""});
    }
    else
    {
        // the 421 is returned.
        CHECK(status_of(out.got[0]) == http::status::MISDIRECTED_REQUEST);
        CHECK(b_seen.bodies() == std::vector<std::string>{""});
    }

    // either way, a.test's connection doesn't take b.test's requests any more.
    CHECK(ok(out.got[1]));
    CHECK(a_seen.count("b.test") == 1);
}

TEST_CASE("a coalesced request's 421 sends it again on its own host's connection, not another coalesced one",
          "[tls][http][client][http2][coalescing]")
{
    dns::resolver resolver{names()};
    served        a_seen;
    served        x_seen;
    served        b_seen;

    // both answer b.test's requests with 421, whichever the request's coalesced onto.
    a_seen.misdirect = 2;
    x_seen.misdirect = 2;

    net::io::scheduler sched{make_pool()};
    auto               la       = listen(&sched);
    auto               lx       = listen(&sched);
    auto               lb       = listen(&sched);
    const auto         pa       = la.local_port();
    const auto         px       = lx.local_port();
    const auto         pb       = lb.local_port();
    const auto         a_server = serve(&sched, std::move(la), &a_seen, {origin("a.test", pa), origin("b.test", pb)});
    const auto         x_server = serve(&sched, std::move(lx), &x_seen, {origin("x.w.test", px), origin("b.test", pb)});
    const auto         b_server = serve(&sched, std::move(lb), &b_seen, {});

    http::client client{&sched, options_for(&resolver, true)};

    const auto out = sched.run_to_completion(
        [](http::client* c, std::uint16_t pa, std::uint16_t px, std::uint16_t pb) -> task<sent>
        {
            // two connections whose Origin Sets have b.test's.
            if (!co_await warm_up(*c, "x.w.test", px)) co_return sent{};
            co_return co_await to_b(c, pa, pb, {request{}});
        }(&client, pa, px, pb));

    REQUIRE(out.warmed);
    REQUIRE(out.got.size() == 1);
    CHECK(ok(out.got[0]));

    // one 421, then b.test's own server.
    CHECK(a_seen.count("b.test") + x_seen.count("b.test") == 1);
    CHECK(b_seen.count("b.test") == 1);
}

namespace
{

// fetch_into fetches url into out, then counts itself done.
task<> fetch_into(http::client*                                           c,
                  std::string                                             url,
                  std::optional<std::expected<fetched, std::error_code>>* out,
                  std::atomic<std::uint64_t>*                             done,
                  net::io::scheduler*                                     sched)
{
    *out = co_await fetch(*c, std::move(url));
    done->fetch_add(1);
    sched->notify_all(*done);
}

}

TEST_CASE("coalesced requests in flight when their origin's removed are each sent again on their own host's "
          "connection",
          "[tls][http][client][http2][coalescing]")
{
    dns::resolver resolver{names()};
    served        a_seen;
    served        b_seen;

    // two of b.test's requests, both held until both have come, then both answered with 421: so the second 421 is
    // for an origin the first has already removed.
    a_seen.misdirect = 2;
    a_seen.hold      = 2;

    std::optional<std::expected<fetched, std::error_code>> first;
    std::optional<std::expected<fetched, std::error_code>> second;
    std::atomic<std::uint64_t>                             done{0};

    net::io::scheduler sched{make_pool()};
    auto               la       = listen(&sched);
    auto               lb       = listen(&sched);
    const auto         pa       = la.local_port();
    const auto         pb       = lb.local_port();
    const auto         a_server = serve(&sched, std::move(la), &a_seen, {origin("a.test", pa), origin("b.test", pb)});
    const auto         b_server = serve(&sched, std::move(lb), &b_seen, {});

    http::client client{&sched, options_for(&resolver, true)};

    const auto warmed = sched.run_to_completion(
        [](http::client*                                           c,
           net::io::scheduler*                                     sched,
           std::uint16_t                                           pa,
           std::uint16_t                                           pb,
           std::optional<std::expected<fetched, std::error_code>>* first,
           std::optional<std::expected<fetched, std::error_code>>* second,
           std::atomic<std::uint64_t>*                             done) -> task<bool>
        {
            if (!co_await warm_up(*c, "a.test", pa)) co_return false;

            if (!sched->spawn(fetch_into(c, url_of("b.test", pb), first, done, sched))) co_return false;
            if (!sched->spawn(fetch_into(c, url_of("b.test", pb), second, done, sched))) co_return false;

            const auto deadline = net::io::clock::now() + 20s;
            for (auto seen = done->load(); seen < 2; seen = done->load())
            {
                if (co_await sched->wait_on(*done, seen, deadline)) break;
            }
            co_return true;
        }(&client, &sched, pa, pb, &first, &second, &done));

    REQUIRE(warmed);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(ok(*first));
    CHECK(ok(*second));

    // both coalesced (and answered 421), then both on b.test's own connection.
    CHECK(a_seen.count("b.test") == 2);
    CHECK(b_seen.count("b.test") == 2);
}

TEST_CASE("a coalesced request's 421 sends it again as a first attempt, which may take an idle connection",
          "[tls][http][client][http2][coalescing]")
{
    dns::resolver resolver{names()};
    served        a_seen;
    served        b_seen;

    // b.test's first request is held, holding a.test's connection's one stream, until the test opens the gate; then
    // answered with 421.
    a_seen.misdirect = 1;
    a_seen.gated     = true;

    std::optional<std::expected<fetched, std::error_code>> first;
    std::atomic<std::uint64_t>                             done{0};

    net::io::scheduler sched{make_pool()};
    auto               la = listen(&sched);
    auto               lb = listen(&sched);
    const auto         pa = la.local_port();
    const auto         pb = lb.local_port();
    const auto         a_server =
        serve(&sched, std::move(la), &a_seen, {origin("a.test", pa), origin("b.test", pb)}, {.max_streams = 1});

    // HTTP/1.1, so its connection's idle between requests.
    const auto b_server = serve(&sched, std::move(lb), &b_seen, {}, {.http2 = false});

    http::client client{&sched, options_for(&resolver, true)};

    struct outcome
    {
        bool                                    ran = false;
        std::expected<fetched, std::error_code> second;
        std::size_t                             b_open = 0;
    };
    const auto out = sched.run_to_completion(
        [](http::client*                                           c,
           net::io::scheduler*                                     sched,
           served*                                                 a_seen,
           std::uint16_t                                           pa,
           std::uint16_t                                           pb,
           std::optional<std::expected<fetched, std::error_code>>* first,
           std::atomic<std::uint64_t>*                             done) -> task<outcome>
        {
            outcome out{.ran = false, .second = std::unexpected(std::error_code{}), .b_open = 0};
            if (!co_await warm_up(*c, "a.test", pa)) co_return out;

            // the first, coalesced, holds a.test's connection's stream; so the second, finding none free there,
            // connects to b.test's server, and leaves its connection idle.
            if (!sched->spawn(fetch_into(c, url_of("b.test", pb), first, done, sched))) co_return out;
            co_await a_seen->reached(a_seen->b_arrived, 1);
            out.second = co_await fetch(*c, url_of("b.test", pb));

            // the first's 421 then sends it again, taking that idle connection rather than making another.
            a_seen->open();
            co_await a_seen->reached(*done, 1);

            out.ran    = true;
            out.b_open = c->stats("b.test", std::to_string(pb)).open;
            co_return out;
        }(&client, &sched, &a_seen, pa, pb, &first, &done));

    REQUIRE(out.ran);
    REQUIRE(first.has_value());
    CHECK(ok(*first));
    CHECK(ok(out.second));

    CHECK(a_seen.count("b.test") == 1);
    CHECK(b_seen.count("b.test") == 2);
    CHECK(out.b_open == 1);
}

namespace
{

// dropping_server is b.test's server, played (test::h2_client in its server role, over TLS with the certificate for
// a.test, b.test and *.w.test, HTTP/2 by ALPN): it ends its first connection (close_notify, then the socket closed, but
// no GOAWAY) once a request's head has come on it, and answers the request on its second. It accepts on its listener
// (which run() owns). NOTE: declared before the scheduler, as its task uses it.
struct dropping_server
{
    std::atomic<int> heads{0}; // requests' heads, on either connection

    static task<> run(dropping_server* self, net::listener listener)
    {
        for (int n = 0; n < 2; ++n)
        {
            auto accepted = co_await listener.accept();
            if (!accepted.has_value()) co_return;

            auto stream = co_await tls::stream::accept(std::move(*accepted),
                                                       server_with(pki().names_chain, pki().names_key),
                                                       net::io::clock::now() + 5s,
                                                       {.alpn = std::vector<std::string>{"h2"}});
            if (!stream.has_value()) co_return;

            http::detail::connection conn{std::move(*stream)};
            test::h2_client          played{&http::detail::as_reader(conn),
                                            &http::detail::as_writer(conn),
                                            &http::detail::socket_of(conn),
                                            http2::role::server};

            const bool head = co_await played.pump([&played] { return !played.responses.empty(); });
            if (!head) co_return;
            ++self->heads;

            if (n == 0)
            {
                co_await http::detail::close(conn);
                continue;
            }

            played.respond(played.responses.begin()->first, "200", "b.test");

            // until the client's done with it (or the scheduler stops).
            (void)co_await played.pump([] { return false; });
        }
    }
};

}

TEST_CASE("a coalesced request's 421 sends it again as a first attempt, which is sent again if its connection drops",
          "[tls][http][client][http2][coalescing]")
{
    dns::resolver resolver{names()};
    served        a_seen;
    a_seen.misdirect = 1;
    dropping_server b_server;

    net::io::scheduler sched{make_pool()};
    auto               lb      = listen(&sched);
    const auto         pb      = lb.local_port();
    const bool         spawned = sched.spawn(dropping_server::run(&b_server, std::move(lb)));
    REQUIRE(spawned);

    auto       la       = listen(&sched);
    const auto pa       = la.local_port();
    const auto a_server = serve(&sched, std::move(la), &a_seen, {origin("a.test", pa), origin("b.test", pb)});

    http::client client{&sched, options_for(&resolver, true)};

    // a.test's server answers 421; b.test's first connection drops once it has the request; its second answers.
    const auto out = sched.run_to_completion(to_b(&client, pa, pb, {request{}}));

    REQUIRE(out.warmed);
    REQUIRE(out.got.size() == 1);
    CAPTURE(out.got[0].has_value() ? std::string{"a response"} : out.got[0].error().message());
    CHECK(ok(out.got[0]));
    CHECK(a_seen.count("b.test") == 1);
    CHECK(b_server.heads.load() == 2);
}
