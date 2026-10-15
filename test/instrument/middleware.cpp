#include "instrument/middleware.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <exception> // IWYU pragma: keep
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "instrument/opentelemetry/protocol.hpp"
#include "instrument/opentelemetry/tracecontext.hpp"
#include "instrument/prometheus/registry.hpp"
#include "io/io.hpp"
#include "io/string_writer.hpp"
#include "util/type_map.hpp"

namespace http          = net::http;
namespace opentelemetry = net::instrument::opentelemetry;

namespace
{

using namespace std::string_view_literals;

using net::coro::task;

http::router test_router()
{
    http::router router;
    router.GET("/users/:id",
               [](const http::server_request&, http::response_writer& resp) -> task<void>
               { co_await resp.send(http::status::OK, 0); });
    router.GET("/silent", [](const http::server_request&, http::response_writer&) -> task<void> { co_return; });
    router.GET("/throws",
               [](const http::server_request&, http::response_writer&) -> task<void>
               {
                   throw std::runtime_error{"oops"};
                   co_return;
               });
    router.GET("/fails",
               [](const http::server_request&, http::response_writer& resp) -> task<void>
               { co_await resp.send(http::status::SERVICE_UNAVAILABLE, 0); });
    return router;
}

// call calls handler with a GET of path, with headers, returning whether it threw.
template<typename Handler>
bool call(const Handler& handler, std::string path, const http::headers& headers = {})
{
    http::server_request req{
        .method  = http::request_method::GET,
        .version = {.major = 1, .minor = 1}
    };
    req.target.path = std::move(path);
    req.headers     = headers;

    net::io::string_writer<char> out;
    http::server_response        base{
        .version = {.major = 1, .minor = 1},
        .body    = &out
    };
    http::response_writer resp{&out, &base, http::http11::response_encode};

    try
    {
        test::sync_wait(handler(req, resp));
        return false;
    }
    catch (const std::runtime_error&)
    {
        return true;
    }
}

std::string record()
{
    net::io::string_writer<char> writer;
    auto                         res = test::sync_wait(net::instrument::prometheus::registry::record(writer));
    REQUIRE_FALSE(res.err);
    return writer.build();
}

std::optional<std::string> attribute(const opentelemetry::span& span, std::string_view key)
{
    auto iter = std::ranges::find(span.attributes, key, &opentelemetry::key_value::key);
    if (iter == span.attributes.end()) return std::nullopt;

    if (const auto* s = std::get_if<std::string>(&iter->value.value)) return *s;
    if (const auto* i = std::get_if<std::int64_t>(&iter->value.value)) return std::to_string(*i);
    return "?";
}

}

// NOTE: one test, as the registry is global: nothing else registers these metrics.
TEST_CASE("http_metrics measures requests, by method, route and status", "[instrument][middleware][metrics]")
{
    const net::instrument::http_metrics handler{test_router()};

    CHECK_FALSE(call(handler, "/users/1"));
    CHECK_FALSE(call(handler, "/users/2"));
    CHECK_FALSE(call(handler, "/nope/1"));
    CHECK_FALSE(call(handler, "/nope/2"));
    CHECK_FALSE(call(handler, "/silent"));
    CHECK(call(handler, "/throws"));

    auto text = record();
    INFO(text);

    CHECK(text.contains("# TYPE http_server_active_requests gauge\nhttp_server_active_requests 0\n"));
    CHECK(text.contains("# TYPE http_server_request_duration_seconds histogram\n"));

    // a route's requests are one series, as are those of no route.
    CHECK(text.contains(
        "http_server_request_duration_seconds_count{code=\"200\",method=\"GET\",route=\"/users/:id\"} 2\n"));
    CHECK(text.contains("http_server_request_duration_seconds_count{code=\"404\",method=\"GET\",route=\"\"} 2\n"));

    // the status the server sends.
    CHECK(
        text.contains("http_server_request_duration_seconds_count{code=\"200\",method=\"GET\",route=\"/silent\"} 1\n"));
    CHECK(
        text.contains("http_server_request_duration_seconds_count{code=\"500\",method=\"GET\",route=\"/throws\"} 1\n"));
}

TEST_CASE("http_tracing makes a server span for each request", "[instrument][middleware][tracing]")
{
    std::vector<opentelemetry::span> spans;

    const net::instrument::http_tracing handler{test_router(),
                                                [&](opentelemetry::span&& s) { spans.push_back(std::move(s)); }};

    SECTION("continuing the request's trace")
    {
        http::headers headers;
        headers.set("traceparent"sv, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00"sv);
        headers.set("tracestate"sv, "vendor=value"sv);

        CHECK_FALSE(call(handler, "/users/42", headers));
        REQUIRE(spans.size() == 1);

        auto parent = opentelemetry::tracecontext::parse(headers);
        REQUIRE(parent.has_value());

        const auto& span = spans[0];
        CHECK(span.trace_id == parent->trace_id);
        CHECK(span.parent_span_id == parent->parent_span_id);
        CHECK(span.span_id != *parent->parent_span_id);
        CHECK(span.trace_state == "vendor=value");
        CHECK(span.flags == opentelemetry::span_flags::zero); // not sampled, as the parent wasn't

        CHECK(span.kind == opentelemetry::span_kind::server);
        CHECK(span.name == "GET /users/:id");
        CHECK(span.start_time <= span.end_time);
        CHECK(span.status.code == opentelemetry::status_code::unset);

        CHECK(attribute(span, "http.request.method") == "GET");
        CHECK(attribute(span, "http.route") == "/users/:id");
        CHECK(attribute(span, "url.path") == "/users/42");
        CHECK(attribute(span, "url.scheme") == "http");
        CHECK(attribute(span, "network.protocol.version") == "1.1");
        CHECK(attribute(span, "http.response.status_code") == "200");
    }

    SECTION("starting a new trace, if the request has none")
    {
        CHECK_FALSE(call(handler, "/nope"));
        REQUIRE(spans.size() == 1);

        const auto& span = spans[0];
        CHECK_FALSE(span.parent_span_id.has_value());
        CHECK(span.trace_id != opentelemetry::trace_id_t{});
        CHECK(span.span_id != opentelemetry::span_id_t{});
        CHECK((span.flags & opentelemetry::span_flags::sampled) == opentelemetry::span_flags::sampled);

        // no route matched, and a 4xx isn't the server's error.
        CHECK(span.name == "GET");
        CHECK_FALSE(attribute(span, "http.route").has_value());
        CHECK(attribute(span, "http.response.status_code") == "404");
        CHECK(span.status.code == opentelemetry::status_code::unset);
    }

    SECTION("a 5xx is an error")
    {
        CHECK_FALSE(call(handler, "/fails"));
        REQUIRE(spans.size() == 1);
        CHECK(spans[0].status.code == opentelemetry::status_code::error);
        CHECK(attribute(spans[0], "http.response.status_code") == "503");
    }

    SECTION("a handler that throws is an error, with its message, and the exception is rethrown")
    {
        CHECK(call(handler, "/throws"));
        REQUIRE(spans.size() == 1);
        CHECK(spans[0].status.code == opentelemetry::status_code::error);
        CHECK(spans[0].status.message == "oops");
        CHECK(attribute(spans[0], "http.response.status_code") == "500");
    }
}

TEST_CASE("handlers can continue the request's trace", "[instrument][middleware][tracing]")
{
    std::vector<opentelemetry::span> spans;
    http::headers                    outgoing;
    bool                             had_trace = false;

    http::router router;
    router.GET("/calls",
               [&](const http::server_request& req, http::response_writer& resp) -> task<void>
               {
                   const auto* trace = req.extensions.get<net::instrument::trace>();
                   had_trace         = trace != nullptr;
                   if (trace == nullptr) co_return;

                   trace->span().attributes.push_back({.key = "app.user", .value = {.value = std::string{"42"}}});

                   // e.g. a request to another service.
                   auto child = trace->start_child("GET users", opentelemetry::span_kind::client);
                   opentelemetry::tracecontext::inject(child, outgoing);
                   trace->end(std::move(child));

                   co_await resp.send(http::status::OK, 0);
               });

    SECTION("not without http_tracing")
    {
        CHECK_FALSE(call(router, "/calls"));
        CHECK_FALSE(had_trace);
    }

    SECTION("with http_tracing")
    {
        const net::instrument::http_tracing handler{std::move(router),
                                                    [&](opentelemetry::span&& s) { spans.push_back(std::move(s)); }};

        http::headers incoming;
        incoming.set("traceparent"sv, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"sv);

        CHECK_FALSE(call(handler, "/calls", incoming));
        REQUIRE(had_trace);

        // the child ends first.
        REQUIRE(spans.size() == 2);
        const auto& child  = spans[0];
        const auto& server = spans[1];

        CHECK(server.kind == opentelemetry::span_kind::server);
        CHECK(attribute(server, "app.user") == "42");

        CHECK(child.kind == opentelemetry::span_kind::client);
        CHECK(child.name == "GET users");
        CHECK(child.trace_id == server.trace_id);
        CHECK(child.parent_span_id == server.span_id);
        CHECK(child.span_id != server.span_id);
        CHECK(child.flags == server.flags);
        CHECK(child.start_time <= child.end_time);

        // the next service continues the trace as the client span's child.
        auto next = opentelemetry::tracecontext::parse(outgoing);
        REQUIRE(next.has_value());
        CHECK(next->trace_id == server.trace_id);
        CHECK(next->parent_span_id == child.span_id);
    }
}

TEST_CASE("the trace is removed from the request once it's handled", "[instrument][middleware][tracing]")
{
    const net::instrument::http_tracing handler{test_router(), [](const opentelemetry::span&) {}};

    http::server_request req{
        .method  = http::request_method::GET,
        .version = {.major = 1, .minor = 1}
    };
    req.target.path = "/users/1";

    net::io::string_writer<char> out;
    http::server_response        base{
        .version = {.major = 1, .minor = 1},
        .body    = &out
    };
    http::response_writer resp{&out, &base, http::http11::response_encode};

    test::sync_wait(handler(req, resp));
    CHECK(req.extensions.get<net::instrument::trace>() == nullptr);
}
