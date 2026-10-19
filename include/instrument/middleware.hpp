#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "instrument/opentelemetry/protocol.hpp"
#include "instrument/prometheus/buckets.hpp"

// HTTP server middleware (see http/middleware.hpp) to measure or trace the requests that the rest of the chain (e.g. a
// router) handles. They're opt-in: e.g.
//
//   http::server server{&scheduler, http::chain(instrument::http_metrics{}, std::move(router))};
//
// Each is a few lines of coroutine around the rest of the chain, calling out-of-line functions before and after it (so
// a chain's inner layers are still elided into its outer one's frame). They rethrow what the rest of the chain throws.
//
// The response status they see is the one the server sends: 200 OK if the rest of the chain didn't send a response, or
// 500 Internal Server Error if it threw before it did. A request canceled before its final response was sent (its
// http::server_request::stop stopped), whatever the rest of the chain did (the server sends no response it didn't send,
// nor a 500), is recorded as 499 Client Closed Request (as nginx logs it) if the client's side stopped it (e.g. it
// reset its HTTP/2 stream, or the connection ended), or 503 Service Unavailable if the server was aborted
// (http::server_request::aborted: abort(), its destructor, or the scheduler's request_stop()). The route is the pattern
// of the route that matched (see http::server_request::route), which a router in the rest of the chain sets, so they
// read it once that returns.
namespace net::instrument
{

struct http_metrics_options
{
    // The request duration histogram's buckets, in seconds.
    std::vector<double> buckets = prometheus::default_buckets();
};

// http_metrics measures the requests the rest of its chain handles, with Prometheus metrics (which are registered when
// it's constructed):
//
//   http_server_active_requests                               Number of HTTP requests being handled.
//   http_server_request_duration_seconds{method,route,code}  Duration of HTTP requests in seconds.
//
// method is the request's method ("_OTHER" if it's unknown), route the pattern of the route that matched (empty if
// none did, so that unmatched paths don't each make a series), and code the response's status. A request's duration
// is until the rest of the chain returns (before the response is flushed), and the histogram's count is the number of
// requests.
//
// Copies share their metrics.
class http_metrics
{
public:
    explicit http_metrics(http_metrics_options options = {});

    template<class Next>
    coro::task<void> operator()(http::server_request& req, http::response_writer& resp, Next& next) const
    {
        const auto start = begin();

        // NOTE: a co_await can't be in a catch block, so a failure is rethrown after it.
        std::exception_ptr failure;
        try
        {
            co_await next(req, resp);
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        end(start, req, resp, failure != nullptr);
        if (failure != nullptr) std::rethrow_exception(failure);
    }

private:
    // begin counts a request as active, and returns when it started.
    [[nodiscard]] std::chrono::steady_clock::time_point begin() const;

    // end records a request that started at start, and whether the rest of the chain threw.
    void end(std::chrono::steady_clock::time_point start,
             const http::server_request&           req,
             const http::response_writer&          resp,
             bool                                  threw) const;

    struct state;
    std::shared_ptr<state> shared;
};

// span_handler is passed each finished span: e.g. to batch them up and export them (see
// opentelemetry::export_trace_service_request). It's called on whichever thread finished the request, and mustn't
// throw.
using span_handler = std::function<void(opentelemetry::span&&)>;

// trace is a handle to the trace of the request being handled, which http_tracing puts in the request's extensions:
//
//   if (const auto* trace = req.extensions.get<instrument::trace>()) ...
//
// e.g. to continue it in a request to another service:
//
//   auto child = trace->start_child("GET users", opentelemetry::span_kind::client);
//   opentelemetry::tracecontext::inject(child, out.headers);
//   auto resp = co_await client.send(out);
//   trace->end(std::move(child));
//
// It's only valid until the rest of the chain returns, when http_tracing removes it.
class trace
{
public:
    trace(opentelemetry::span* server_span, const span_handler* on_end) noexcept
        : server_span{server_span}
        , on_end{on_end}
    {}

    // span is the request's server span, which ends when the handler returns. e.g. a handler can add attributes or
    // events to it.
    //
    // NOTE: it isn't synchronized: it mustn't be changed by more than one thread at once.
    [[nodiscard]] opentelemetry::span& span() const noexcept { return *server_span; }

    // start_child starts a child of the server span, in the same trace: e.g. a client span, for a request to another
    // service (whose trace context headers opentelemetry::tracecontext::inject sets).
    [[nodiscard]] opentelemetry::span start_child(std::string name, opentelemetry::span_kind kind) const;

    // end ends a child span (unless its end time is already set), and passes it on as the server span will be.
    void end(opentelemetry::span&& child) const;

private:
    opentelemetry::span* server_span;
    const span_handler*  on_end;
};

// http_tracing makes a server span for each request the rest of its chain handles: the child of the span in the
// request's trace
// context (its traceparent and tracestate headers, see opentelemetry::tracecontext), or else the root of a new trace,
// which is sampled. Every finished span is passed to on_end, sampled or not (its flags say).
//
// Each span is named "{method} {route}" (or "{method}", if no route matched), and has OpenTelemetry's HTTP server
// attributes: http.request.method, http.route, url.path, url.scheme, network.protocol.version and
// http.response.status_code. Its status is an error if the response is a 5xx, or if the handler threw (with the
// exception's message), unless the client's side canceled the request (a 499, which is the client's doing, as a 4xx
// is); one the server's abort canceled is a 503, an error.
//
// The handler can get the trace from the request's extensions (see trace): e.g. to propagate it to the requests it
// makes.
//
// Copies share on_end.
class http_tracing
{
public:
    explicit http_tracing(span_handler on_end);

    template<class Next>
    coro::task<void> operator()(http::server_request& req, http::response_writer& resp, Next& next) const
    {
        opentelemetry::span span;
        begin(req, span);

        std::exception_ptr failure;
        try
        {
            co_await next(req, resp);
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        end(req, resp, std::move(span), failure);
        if (failure != nullptr) std::rethrow_exception(failure);
    }

private:
    // begin starts span, the request's server span, and puts its trace in the request's extensions.
    void begin(http::server_request& req, opentelemetry::span& span) const;

    // end removes the trace from the request's extensions, ends span (with what the rest of the chain did, and the
    // exception it threw, if it did), and passes it to on_end.
    void end(http::server_request&        req,
             const http::response_writer& resp,
             opentelemetry::span&&        span,
             const std::exception_ptr&    failure) const;

    std::shared_ptr<const span_handler> on_end;
};

}
