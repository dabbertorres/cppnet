#include "http/server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/callback_sink.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/connection_pool.hpp"
#include "http/h2_client.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "tls/certificate.hpp"
#include "tls/test_pki.hpp"
#include "tls/tls.hpp"
#include "tls/version.hpp"

namespace http = net::http;
namespace tls  = net::tls;

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using net::coro::task;
using test::pki;
using test::server_with;
using test::trusting_client;

namespace
{

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(4); }

// version_name is how a test's captures (and tls_version's responses) name a TLS version.
std::string_view version_name(tls::version v)
{
    switch (v)
    {
    case tls::version::none: return "none";
    case tls::version::tls12: return "TLS 1.2";
    case tls::version::tls13: return "TLS 1.3";
    }
    return "unknown";
}

// tls_version answers with the TLS version the request's connection agreed on.
task<void> tls_version(const http::server_request& req, http::response_writer& resp)
{
    const auto msg  = version_name(req.tls_version);
    auto*      body = co_await resp.send(http::status::OK, msg.size());
    co_await net::io::write_all(*body, msg);
}

// peer_certificates answers with how many certificates the client sent, and the first's subject: "none" if it sent
// none.
task<void> peer_certificates(const http::server_request& req, http::response_writer& resp)
{
    const auto& certs = req.peer_certificates;
    const auto  msg   = certs ? std::to_string(certs->size()) + " " + certs->front().subject : std::string{"none"};
    auto*       body  = co_await resp.send(http::status::OK, msg.size());
    co_await net::io::write_all(*body, msg);
}

// hello says whether the request came over TLS.
task<void> hello(const http::server_request& req, http::response_writer& resp)
{
    const auto msg  = req.tls_version != tls::version::none ? "hello over TLS"sv : "hello over TCP"sv;
    auto*      body = co_await resp.send(http::status::OK, msg.size());
    co_await net::io::write_all(*body, msg);
}

// log_watch keeps what a server logs, for a test to wait for.
struct log_watch
{
    explicit log_watch(net::io::scheduler* sched)
        : sched{sched}
    {
        logger->set_level(spdlog::level::trace);
    }

    // wait waits (up to 5s) until a line containing text has been logged, returning whether one was.
    task<bool> wait(std::string_view text)
    {
        const auto deadline = net::io::clock::now() + 5s;
        while (true)
        {
            const auto seen = count.load();
            if (contains(text)) co_return true;
            if (co_await sched->wait_on(count, seen, deadline)) co_return false;
        }
    }

    bool contains(std::string_view text)
    {
        std::scoped_lock lock{mu};
        return std::ranges::any_of(lines, [text](const std::string& line) { return line.contains(text); });
    }

    net::io::scheduler*        sched;
    std::mutex                 mu;
    std::vector<std::string>   lines;
    std::atomic<std::uint64_t> count = 0;

    std::shared_ptr<spdlog::logger> logger =
        std::make_shared<spdlog::logger>("tls-server",
                                         std::make_shared<spdlog::sinks::callback_sink_mt>(
                                             [this](const spdlog::details::log_msg& msg)
                                             {
                                                 {
                                                     std::scoped_lock lock{mu};
                                                     lines.emplace_back(msg.payload.data(), msg.payload.size());
                                                 }
                                                 count.fetch_add(1);
                                                 sched->notify_all(count);
                                             }));
};

// https_server is a server of hello, over TLS (if tls is set), on a port of its own.
struct https_server
{
    https_server(net::io::scheduler* sched, const http::server_config& cfg)
        : https_server{sched, cfg, router()}
    {}

    https_server(net::io::scheduler* sched, const http::server_config& cfg, http::router r)
        : server{sched, net::listener::make(sched, "127.0.0.1", "0").value(), std::move(r), cfg}
    {
        REQUIRE(sched->spawn(server.serve()));
    }

    static http::router router()
    {
        http::router r;
        r.GET("/", hello);
        r.GET("/tls-version", tls_version);
        r.GET("/peer-certificates", peer_certificates);
        return r;
    }

    [[nodiscard]] std::string port() const { return std::to_string(server.local_port()); }

    http::server server;
};

http::server_config tls_config(std::chrono::seconds header_read_timeout = 5s)
{
    return {.header_read_timeout = header_read_timeout, .tls = server_with(pki().localhost_chain, pki().localhost_key)};
}

// tls_config_up_to is tls_config, with a server that allows TLS versions up to max_version.
http::server_config tls_config_up_to(tls::version max_version)
{
    auto ctx = tls::server_context::make(
        {.certificate_chain = pki().localhost_chain, .private_key = pki().localhost_key, .max_version = max_version});
    REQUIRE(ctx.has_value());
    return {.header_read_timeout = 5s, .tls = std::move(*ctx)};
}

http::client_options trusting_options(tls::version max_version = tls::version::tls13)
{
    http::client_options options;
    options.tls.trusted_ca_file = pki().ca;
    options.tls.max_version     = max_version;
    return options;
}

// fetch GETs url, returning the body, or the error.
task<std::string> fetch(http::client* client, std::string url)
{
    auto resp = co_await client->get(url);
    if (!resp.has_value()) co_return "error: " + resp.error().message();

    auto body = co_await net::io::read_all(*resp->body);
    co_return body.has_value() ? *body : "error: " + body.error().message();
}

// read_to_end reads s until it ends (or fails), returning what it read and why it stopped.
task<std::pair<std::string, std::error_code>> read_to_end(net::io::reader& s)
{
    std::string                  out;
    std::array<std::byte, 4'096> buf{};
    while (true)
    {
        auto res = co_await s.read(buf);
        out.append(reinterpret_cast<const char*>(buf.data()), res.count);
        if (res.err) co_return std::pair{out, res.err};
    }
}

}

TEST_CASE("an HTTPS server serves requests over TLS", "[tls][http][server]")
{
    net::io::scheduler sched{make_pool()};

    const bool use_tls = GENERATE(true, false);
    CAPTURE(use_tls);

    https_server      server{&sched, use_tls ? tls_config() : http::server_config{}};
    http::client      client{&sched, trusting_options()};
    const std::string url = (use_tls ? "https://localhost:" : "http://localhost:") + server.port() + "/";

    struct outcome
    {
        std::string            first;
        std::string            second;
        http::connection_stats stats;
    };

    // NOTE: the stats while the scheduler runs: stopping it ends an HTTP/2 connection (which TLS agrees on).
    const auto test = [](http::client* client, std::string url) -> task<outcome>
    {
        auto first  = co_await fetch(client, url);
        auto second = co_await fetch(client, url);
        co_return outcome{.first = first, .second = second, .stats = client->stats()};
    };

    const auto out = sched.run_to_completion(test(&client, url));

    const std::string_view expected = use_tls ? "hello over TLS" : "hello over TCP";
    CHECK(out.first == expected);
    CHECK(out.second == expected);

    // the connection was kept for the second request.
    CHECK(out.stats == http::connection_stats{.open = 1, .idle = 1});
}

TEST_CASE("an HTTPS server serves HTTP/1.1 to clients that don't speak HTTP/2", "[tls][http][server][client]")
{
    // and the client's pool reuses the idle connection (once tls::detail::idle_clean() says it may), over either
    // version: the client allows TLS 1.2 only, or 1.3 too.
    const auto max_version = GENERATE(tls::version::tls13, tls::version::tls12);
    CAPTURE(version_name(max_version));

    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, tls_config()};

    auto options  = trusting_options(max_version);
    options.http2 = false;
    http::client client{&sched, options};

    struct outcome
    {
        std::vector<std::string> responses;
        http::connection_stats   stats;
    };

    const auto test = [](http::client* client, std::string url) -> task<outcome>
    {
        outcome out;
        for (int i = 0; i < 2; ++i)
        {
            auto resp = co_await client->get(url);
            if (!resp.has_value())
            {
                out.responses.push_back("error: " + resp.error().message());
                continue;
            }

            auto body = co_await net::io::read_all(*resp->body);
            out.responses.push_back(std::format("HTTP/{}.{} {}",
                                                resp->version.major,
                                                resp->version.minor,
                                                body.has_value() ? *body : "error: " + body.error().message()));
        }

        out.stats = client->stats();
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&client, "https://localhost:" + server.port() + "/"));
    REQUIRE(out.responses.size() == 2);
    CHECK(out.responses[0] == "HTTP/1.1 hello over TLS");
    CHECK(out.responses[1] == "HTTP/1.1 hello over TLS");

    // the connection was kept for the second request.
    CHECK(out.stats == http::connection_stats{.open = 1, .idle = 1});
}

TEST_CASE("the HTTP client speaks HTTP/2 to an HTTPS server that does", "[tls][http][client][http2]")
{
    // agreed on with ALPN, over either version: the server allows TLS 1.2 only, or 1.3 too.
    const auto max_version = GENERATE(tls::version::tls13, tls::version::tls12);
    CAPTURE(version_name(max_version));

    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, tls_config_up_to(max_version)};
    http::client       client{&sched, trusting_options()};

    const auto test = [](http::client* client, std::string url) -> task<std::string>
    {
        auto resp = co_await client->get(url);
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        auto body = co_await net::io::read_all(*resp->body);
        if (!body.has_value()) co_return "error: " + body.error().message();
        co_return std::format("HTTP/{} {}", resp->version.major, *body);
    };

    CHECK(sched.run_to_completion(test(&client, "https://localhost:" + server.port() + "/"))
          == "HTTP/2 hello over TLS");
}

TEST_CASE("a handler sees the TLS version its connection agreed on", "[tls][http][server][version]")
{
    struct test_case
    {
        // none: cleartext.
        tls::version version = tls::version::none;
        bool         http2   = false;
    };

    const auto tc = GENERATE(test_case{.version = tls::version::tls13, .http2 = false},
                             test_case{.version = tls::version::tls13, .http2 = true},
                             test_case{.version = tls::version::tls12, .http2 = false},
                             test_case{.version = tls::version::tls12, .http2 = true},
                             test_case{.version = tls::version::none, .http2 = false},
                             test_case{.version = tls::version::none, .http2 = true});
    CAPTURE(version_name(tc.version), tc.http2);

    const bool use_tls = tc.version != tls::version::none;

    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, use_tls ? tls_config() : http::server_config{}};

    // the client allows only the version (the server both): over TLS, HTTP/2 is agreed on with ALPN, and in
    // cleartext spoken with prior knowledge.
    auto options = trusting_options();
    if (use_tls)
    {
        options.tls.min_version = tc.version;
        options.tls.max_version = tc.version;
    }
    options.http2                 = tc.http2;
    options.http2_prior_knowledge = tc.http2 && !use_tls;
    http::client client{&sched, options};

    const auto test = [](http::client* client, std::string url) -> task<std::string>
    {
        auto resp = co_await client->get(url);
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        auto body = co_await net::io::read_all(*resp->body);
        if (!body.has_value()) co_return "error: " + body.error().message();
        co_return std::format("HTTP/{} {}", resp->version.major, *body);
    };

    const std::string url = (use_tls ? "https://localhost:" : "http://localhost:") + server.port() + "/tls-version";
    const auto        out = sched.run_to_completion(test(&client, url));
    CHECK(out == std::format("HTTP/{} {}", tc.http2 ? 2 : 1, version_name(tc.version)));
}

TEST_CASE("a handler sees the certificate its client sent", "[tls][http][server][client_auth]")
{
    struct test_case
    {
        std::string_view name;
        // none: cleartext.
        tls::version     version     = tls::version::tls13;
        bool             http2       = false;
        tls::client_auth auth        = tls::client_auth::required;
        bool             client_cert = true;
        bool             sees        = true;
    };

    const auto tc = GENERATE(
        test_case{.name = "TLS 1.3, HTTP/1.1"},
        test_case{.name = "TLS 1.3, HTTP/2", .http2 = true},
        test_case{.name = "TLS 1.2, HTTP/1.1", .version = tls::version::tls12},
        test_case{.name = "TLS 1.2, HTTP/2", .version = tls::version::tls12, .http2 = true},
        test_case{.name        = "optional, without one",
                  .auth        = tls::client_auth::optional,
                  .client_cert = false,
                  .sees        = false},
        test_case{.name        = "optional, without one, HTTP/2",
                  .http2       = true,
                  .auth        = tls::client_auth::optional,
                  .client_cert = false,
                  .sees        = false},
        test_case{.name = "not asked for", .auth = tls::client_auth::none, .sees = false},
        test_case{.name = "not asked for, HTTP/2", .http2 = true, .auth = tls::client_auth::none, .sees = false},
        test_case{.name = "cleartext", .version = tls::version::none, .sees = false},
        test_case{.name = "cleartext, HTTP/2", .version = tls::version::none, .http2 = true, .sees = false});
    CAPTURE(tc.name);

    const bool use_tls = tc.version != tls::version::none;

    net::io::scheduler  sched{make_pool()};
    http::server_config cfg{.header_read_timeout = 5s};
    if (use_tls) cfg.tls = test::client_auth_server(tc.auth);
    https_server server{&sched, cfg};

    // the client allows only the version (the server both).
    auto options = trusting_options();
    if (use_tls)
    {
        options.tls.min_version = tc.version;
        options.tls.max_version = tc.version;
    }
    if (tc.client_cert)
    {
        options.tls.certificate_chain = pki().client_chain;
        options.tls.private_key       = pki().client_key;
    }
    options.http2                 = tc.http2;
    options.http2_prior_knowledge = tc.http2 && !use_tls;
    http::client client{&sched, options};

    const auto test = [](http::client* client, std::string url) -> task<std::string>
    {
        auto resp = co_await client->get(url);
        if (!resp.has_value()) co_return "error: " + resp.error().message();

        auto body = co_await net::io::read_all(*resp->body);
        if (!body.has_value()) co_return "error: " + body.error().message();
        co_return std::format("HTTP/{} {}", resp->version.major, *body);
    };

    const std::string url =
        (use_tls ? "https://localhost:" : "http://localhost:") + server.port() + "/peer-certificates";
    const auto out = sched.run_to_completion(test(&client, url));
    CHECK(out == std::format("HTTP/{} {}", tc.http2 ? 2 : 1, tc.sees ? "1 " + pki().client_subject : "none"));
}

TEST_CASE("requests to a server that requires a client certificate fail without one that verifies",
          "[tls][http][server][client][client_auth]")
{
    // over TLS 1.3, the client's handshake is done before the server looks at its certificate, so the request fails
    // reading its response (HTTP/1.1), or the connection's first read (HTTP/2); over TLS 1.2, connecting does.
    const auto version     = GENERATE(tls::version::tls13, tls::version::tls12);
    const bool http2       = GENERATE(false, true);
    const bool client_cert = GENERATE(false, true); // an expired one
    CAPTURE(version_name(version), http2, client_cert);

    net::io::scheduler sched{make_pool()};
    https_server       server{
        &sched,
        {.header_read_timeout = 5s, .tls = test::client_auth_server(tls::client_auth::required)}
    };

    auto options            = trusting_options(version);
    options.tls.min_version = version;
    options.http2           = http2;
    if (client_cert)
    {
        options.tls.certificate_chain = pki().expired_client_chain;
        options.tls.private_key       = pki().expired_client_key;
    }
    http::client client{&sched, options};

    const auto test = [](http::client* client, std::string url) -> task<std::error_code>
    {
        auto resp = co_await client->get(url);
        if (!resp.has_value()) co_return resp.error();

        auto body = co_await net::io::read_all(*resp->body);
        co_return body.has_value() ? std::error_code{} : body.error();
    };

    const auto err = sched.run_to_completion(test(&client, "https://localhost:" + server.port() + "/"));
    CAPTURE(err.message());

    const auto alert = test::refusal(client_cert, version);
    CHECK(err == alert);
    CHECK(tls::from_peer(err));
}

TEST_CASE("an HTTPS server resumes its clients' sessions", "[tls][http][server][client][resumption]")
{
    struct test_case
    {
        std::string_view name;
        bool             http2       = false;
        tls::version     version     = tls::version::tls13;
        bool             client_cert = false;
    };

    const auto tc = GENERATE(test_case{.name = "HTTP/1.1"},
                             test_case{.name = "HTTP/2", .http2 = true},
                             test_case{.name = "HTTP/1.1, a client certificate", .client_cert = true},
                             test_case{.name = "HTTP/2, a client certificate", .http2 = true, .client_cert = true},
                             test_case{.name = "TLS 1.2", .version = tls::version::tls12},
                             test_case{.name = "TLS 1.2, HTTP/2", .http2 = true, .version = tls::version::tls12});
    CAPTURE(tc.name);

    // whether each of the server's handshakes resumed a session. NOTE: before the scheduler, as its tasks use it.
    std::mutex        mu;
    std::vector<bool> handshakes;

    net::io::scheduler sched{make_pool()};

    tls::server_options options{.certificate_chain = pki().localhost_chain, .private_key = pki().localhost_key};
    if (tc.client_cert)
    {
        options.client_auth    = tls::client_auth::required;
        options.client_ca_file = pki().client_ca;
    }
    tls::detail::server_hooks hooks{.handshake_done = [&mu, &handshakes](bool resumed)
                                    {
                                        std::scoped_lock lock{mu};
                                        handshakes.push_back(resumed);
                                    }};
    auto                      ctx = tls::detail::make_server_context(options, std::move(hooks));
    REQUIRE(ctx.has_value());
    https_server server{
        &sched,
        {.header_read_timeout = 5s, .tls = std::move(*ctx)}
    };

    // NOTE: no idle connections, so each request makes its own.
    auto client_options         = trusting_options(tc.version);
    client_options.http2        = tc.http2;
    client_options.idle_timeout = 0ms;
    if (tc.client_cert)
    {
        client_options.tls.certificate_chain = pki().client_chain;
        client_options.tls.private_key       = pki().client_key;
    }
    http::client client{&sched, client_options};

    const auto test = [](http::client* client, std::string url) -> task<std::vector<std::string>>
    {
        std::vector<std::string> out;
        for (int i = 0; i < 3; ++i)
        {
            auto resp = co_await client->get(url);
            if (!resp.has_value())
            {
                out.push_back("error: " + resp.error().message());
                continue;
            }

            auto body = co_await net::io::read_all(*resp->body);
            out.push_back(std::format("HTTP/{} {}",
                                      resp->version.major,
                                      body.has_value() ? *body : "error: " + body.error().message()));
        }
        co_return out;
    };

    const auto out =
        sched.run_to_completion(test(&client, "https://localhost:" + server.port() + "/peer-certificates"));

    // a resumed session's handler sees the client's certificate too: the session's.
    const auto expected =
        std::format("HTTP/{} {}", tc.http2 ? 2 : 1, tc.client_cert ? "1 " + pki().client_subject : "none");
    CHECK(out == std::vector<std::string>(3, expected));

    std::scoped_lock lock{mu};
    if (tc.version == tls::version::tls13) CHECK(handshakes == std::vector{false, true, true});
    else CHECK(handshakes == std::vector{false, false, false});
}

TEST_CASE("a burst of requests to a host whose connection agreed on HTTP/2 shares the connection being made",
          "[tls][http][client][http2]")
{
    net::io::scheduler sched{make_pool()};
    test::gate         g{&sched};

    // responses whose bodies are held, to fill a connection's streams.
    auto r = https_server::router();
    r.GET("/held",
          [&g](const http::server_request&, http::response_writer& resp) -> task<void>
          {
              auto* body = co_await resp.send(http::status::OK, 4);
              co_await g.pass();
              co_await net::io::write_all(*body, "held"sv);
          });

    auto cfg                         = tls_config();
    cfg.http2_max_concurrent_streams = 2;
    https_server server{&sched, cfg, std::move(r)};
    http::client client{&sched, trusting_options()};

    const auto test = [](net::io::scheduler* sched, http::client* client, test::gate* g, std::string base)
        -> task<std::vector<std::string>>
    {
        // the connection agrees on HTTP/2, and its two streams are taken.
        auto first  = co_await client->get(base + "/held");
        auto second = co_await client->get(base + "/held");
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        std::vector<std::string> out{std::format("open {}", client->stats().open)};

        // then a burst of two: the first connects, and the second, as the host's known to speak HTTP/2 (its connection
        // is still there), waits for that connection, rather than making its own.
        constexpr int              n = 2;
        std::vector<std::string>   results(n);
        std::atomic<std::uint64_t> done{0};
        for (int i = 0; i < n; ++i)
        {
            REQUIRE(sched->spawn(
                [](net::io::scheduler*         sched,
                   http::client*               client,
                   std::string                 url,
                   std::string*                result,
                   std::atomic<std::uint64_t>* done) -> task<>
                {
                    *result = co_await fetch(client, url);
                    done->fetch_add(1);
                    sched->notify_all(*done);
                }(sched, client, base + "/", &results[static_cast<std::size_t>(i)], &done)));
        }

        for (auto seen = done.load(); seen < n; seen = done.load()) (void)co_await sched->wait_on(done, seen);
        out.insert(out.end(), results.begin(), results.end());
        out.push_back(std::format("open {}", client->stats().open));

        g->open();
        g->open();
        for (auto* held : {&*first, &*second})
        {
            auto body = co_await net::io::read_all(*held->body);
            out.push_back(body.value_or("error"));
        }
        co_return out;
    };

    CHECK(sched.run_to_completion(test(&sched, &client, &g, "https://localhost:" + server.port()))
          == std::vector<std::string>{"open 1", "hello over TLS", "hello over TLS", "open 2", "held", "held"});
}

TEST_CASE("an HTTPS server closes connections that don't speak TLS", "[tls][http][server]")
{
    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, tls_config()};
    http::client       client{&sched, trusting_options()};

    const auto test = [](http::client* client, std::string port) -> task<std::pair<std::string, std::string>>
    {
        auto plain = co_await fetch(client, "http://localhost:" + port + "/");
        co_return std::pair{plain, co_await fetch(client, "https://localhost:" + port + "/")};
    };

    auto [plain, over_tls] = sched.run_to_completion(test(&client, server.port()));
    CHECK(plain.starts_with("error: "));

    // and still serves those that do.
    CHECK(over_tls == "hello over TLS");
}

TEST_CASE("an HTTPS server closes connections whose handshake doesn't finish in time", "[tls][http][server]")
{
    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, tls_config(1s)};

    // connects, but never starts the handshake.
    const auto test = [](net::io::scheduler* sched, std::string port) -> task<std::error_code>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        if (!conn.has_value()) co_return conn.error();

        std::array<std::byte, 64> buf{};
        auto                      res = co_await conn->read(buf);
        co_return res.err;
    };

    const auto start = std::chrono::steady_clock::now();
    auto       err   = sched.run_to_completion(test(&sched, server.port()));
    CAPTURE(err.message());
    CHECK(err == net::io::status_condition::closed);
    CHECK(std::chrono::steady_clock::now() - start < 4s);
}

TEST_CASE("closing an HTTPS server ends handshakes in progress", "[tls][http][server]")
{
    net::io::scheduler sched{make_pool()};
    log_watch          log{&sched};

    // without header_read_timeout, nothing but close() ends a handshake that doesn't finish.
    auto cfg   = tls_config(0s);
    cfg.logger = log.logger;
    https_server server{&sched, cfg};

    struct outcome
    {
        bool            accepted = false;
        std::error_code read_err;
        bool            canceled = false;
    };

    // connects, but never starts the handshake; then closes the server, once it has accepted the connection.
    const auto test = [](net::io::scheduler* sched, http::server* server, log_watch* log) -> task<outcome>
    {
        outcome out;

        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", std::to_string(server->local_port()));
        if (!conn.has_value())
        {
            out.read_err = conn.error();
            co_return out;
        }

        out.accepted = co_await log->wait("connection accepted");
        server->close();

        std::array<std::byte, 64> buf{};
        auto                      res = co_await conn->read(buf);
        out.read_err                  = res.err;
        out.canceled                  = co_await log->wait("TLS handshake failed: Operation canceled");
        co_return out;
    };

    const auto start = std::chrono::steady_clock::now();
    auto       out   = sched.run_to_completion(test(&sched, &server.server, &log));
    CAPTURE(out.read_err.message());
    CHECK(out.accepted);
    CHECK(out.read_err == net::io::status_condition::closed);
    CHECK(out.canceled);
    CHECK(std::chrono::steady_clock::now() - start < 4s);
}

TEST_CASE("an HTTPS server serves HTTP/2 to clients that agree on h2", "[tls][http][server][http2]")
{
    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, tls_config()};

    const auto test = [](net::io::scheduler* sched, std::string port) -> task<std::pair<std::string, std::string>>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        REQUIRE(conn.has_value());

        auto s = co_await tls::stream::connect(std::move(*conn),
                                               trusting_client({"h2", "http/1.1"}),
                                               "localhost",
                                               net::io::clock::now() + 5s);
        REQUIRE(s.has_value());
        if (s->alpn() != "h2") co_return std::pair{"ALPN: " + std::string{s->alpn()}, std::string{}};

        test::h2_client client{&*s, &*s, &s->socket()};
        const auto      id = client.request("GET", "/");
        REQUIRE(co_await client.wait_for(id));

        co_await s->close();
        co_return std::pair{client.responses[id].status, client.responses[id].body};
    };

    auto [status, body] = sched.run_to_completion(test(&sched, server.port()));
    CHECK(status == "200");
    CHECK(body == "hello over TLS");
}

TEST_CASE("without http2, an HTTPS server doesn't agree on h2", "[tls][http][server][http2]")
{
    net::io::scheduler sched{make_pool()};

    auto cfg  = tls_config();
    cfg.http2 = false;
    https_server server{&sched, cfg};

    const auto test = [](net::io::scheduler* sched, std::string port) -> task<std::string>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        REQUIRE(conn.has_value());

        auto s = co_await tls::stream::connect(std::move(*conn),
                                               trusting_client({"h2"}),
                                               "localhost",
                                               net::io::clock::now() + 5s);
        REQUIRE(s.has_value());

        const std::string alpn{s->alpn()};
        co_await s->close();
        co_return alpn;
    };

    CHECK(sched.run_to_completion(test(&sched, server.port())).empty());
}

TEST_CASE("an HTTPS server's error responses end with close_notify", "[tls][http][server]")
{
    net::io::scheduler sched{make_pool()};
    https_server       server{&sched, tls_config()};

    const auto test = [](net::io::scheduler* sched, std::string port) -> task<std::pair<std::string, std::error_code>>
    {
        auto conn = co_await net::tcp_socket::connect(sched, "127.0.0.1", port);
        if (!conn.has_value()) co_return std::pair{"connect failed", conn.error()};

        auto s =
            co_await tls::stream::connect(std::move(*conn), trusting_client(), "localhost", net::io::clock::now() + 5s);
        if (!s.has_value()) co_return std::pair{"handshake failed", s.error()};

        co_await net::io::write_all(*s, "NOT HTTP\r\n\r\n"sv);
        co_return co_await read_to_end(*s);
    };

    auto [received, err] = sched.run_to_completion(test(&sched, server.port()));
    CAPTURE(received, err.message());
    CHECK(received.starts_with("HTTP/1.1 400 "));
    CHECK(err == net::io::errc::end_of_stream);
}

TEST_CASE("closing an HTTPS server lets a request in progress finish", "[tls][http][server][close]")
{
    net::io::scheduler sched{make_pool()};

    // set once the handler's started, before it reads the body.
    std::atomic<std::uint64_t> started = 0;

    http::router router;
    router.POST("/",
                [&](http::server_request& req, http::response_writer& resp) -> task<void>
                {
                    started.store(1);
                    sched.notify_all(started);

                    auto body = co_await net::io::read_all(*req.body);

                    const std::string out = body.has_value() ? "whole: " + *body : "failed: " + body.error().message();
                    auto*             writer = co_await resp.send(http::status::OK, out.size());
                    co_await net::io::write_all(*writer, out);
                });

    http::server server{&sched, net::listener::make(&sched, "127.0.0.1", "0").value(), std::move(router), tls_config()};
    REQUIRE(sched.spawn(server.serve()));

    // half the body, then the rest once the server's closed, as its handler reads it.
    const auto test = [&] -> task<std::pair<std::string, std::error_code>>
    {
        auto conn = co_await net::tcp_socket::connect(&sched, "127.0.0.1", std::to_string(server.local_port()));
        if (!conn.has_value()) co_return std::pair{"connect failed", conn.error()};

        auto s = co_await tls::stream::connect(std::move(*conn),
                                               trusting_client({"http/1.1"}),
                                               "localhost",
                                               net::io::clock::now() + 5s);
        if (!s.has_value()) co_return std::pair{"handshake failed", s.error()};

        (void)co_await net::io::write_all(*s, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nhello"sv);

        const auto deadline = net::io::clock::now() + 5s;
        while (started.load() == 0)
        {
            if (co_await sched.wait_on(started, 0, deadline)) co_return std::pair{"not started", std::error_code{}};
        }

        server.close();
        (void)co_await net::io::write_all(*s, "world"sv);
        co_return co_await read_to_end(*s);
    };

    auto [received, err] = sched.run_to_completion(test());
    CAPTURE(received, err.message());

    // and then close_notify.
    CHECK(received.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(received.contains("\r\nConnection: close\r\n"));
    CHECK(received.ends_with("\r\n\r\nwhole: helloworld"));
    CHECK(err == net::io::errc::end_of_stream);
}

TEST_CASE("an HTTPS client leaving stops its waiting handler's token, and is still answered, unless by TLS 1.2's "
          "close_notify",
          "[tls][http][server][cancel][watch]")
{
    // NOTE: before the scheduler, as the handler sets them.
    std::atomic<int> stopped = -1;

    net::io::scheduler sched{make_pool()};

    http::router router;
    router.GET("/wait",
               [&](const http::server_request& req, http::response_writer& resp) -> task<void>
               {
                   const auto err = co_await sched.sleep_until(net::io::clock::now() + 5s, req.stop);
                   stopped.store(err ? 1 : 0);

                   const auto msg  = err ? "stopped"sv : "slept"sv;
                   auto*      body = co_await resp.send(http::status::OK, msg.size());
                   co_await net::io::write_all(*body, msg);
               });

    http::server server{&sched, net::listener::make(&sched, "127.0.0.1", "0").value(), std::move(router), tls_config()};
    REQUIRE(sched.spawn(server.serve()));

    // with close_notify (which, over TLS 1.3, the server answers only once it has responded), or just the end of the
    // TCP stream (a TLS stream cut short).
    const bool close_notify = GENERATE(true, false);
    CAPTURE(close_notify);

    // over TLS 1.3, or 1.2 (the client allowing only that), whose close_notify Botan answers at once.
    const auto version = GENERATE(tls::version::tls13, tls::version::tls12);
    CAPTURE(version_name(version));

    auto ctx = tls::client_context::make({.trusted_ca_file = pki().ca, .alpn = {"http/1.1"}, .max_version = version});
    REQUIRE(ctx.has_value());

    const auto test = [&] -> task<std::pair<std::string, std::error_code>>
    {
        auto conn = co_await net::tcp_socket::connect(&sched, "127.0.0.1", std::to_string(server.local_port()));
        if (!conn.has_value()) co_return std::pair{"connect failed", conn.error()};

        auto s = co_await tls::stream::connect(std::move(*conn), *ctx, "localhost", net::io::clock::now() + 5s);
        if (!s.has_value()) co_return std::pair{"handshake failed", s.error()};
        if (s->version() != version) co_return std::pair{"another version", std::error_code{}};

        (void)co_await net::io::write_all(*s, "GET /wait HTTP/1.1\r\nHost: x\r\n\r\n"sv);

        if (close_notify) (void)co_await s->shutdown();
        else s->socket().shutdown_write();

        co_return co_await read_to_end(*s);
    };

    const auto start     = std::chrono::steady_clock::now();
    auto [received, err] = sched.run_to_completion(test());
    CAPTURE(received, err.message());

    CHECK(stopped.load() == 1);
    CHECK(std::chrono::steady_clock::now() - start < 3s);

    // over TLS 1.2, the server answered the client's close_notify as it read it, so it couldn't respond: only its
    // close_notify came.
    if (close_notify && version == tls::version::tls12)
    {
        CHECK(received.empty());
    }
    else
    {
        // and then close_notify.
        CHECK(received.starts_with("HTTP/1.1 200 OK\r\n"));
        CHECK(received.ends_with("\r\n\r\nstopped"));
    }
    CHECK(err == net::io::errc::end_of_stream);
}
