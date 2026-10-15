#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "instrument/opentelemetry/protocol.hpp"
#include "instrument/prometheus/buckets.hpp"

// HTTP server middleware: request handlers that wrap another (e.g. a router) to measure or trace the requests it
// handles. They're opt-in: e.g.
//
//   http::server server{&scheduler, instrument::http_metrics{std::move(router)}};
//
// The response status they see is the one the server sends: 200 OK if the handler didn't send a response, or 500
// Internal Server Error if it threw before it did. The route is the pattern of the route that matched (see
// http::server_request::route): wrapping a router, rather than being wrapped by one, means it's been set.
namespace net::instrument
{

struct http_metrics_options
{
    // The request duration histogram's buckets, in seconds.
    std::vector<double> buckets = prometheus::default_buckets();
};

// http_metrics measures the requests its handler handles, with Prometheus metrics (which are registered when it's
// constructed):
//
//   http_server_active_requests                               Number of HTTP requests being handled.
//   http_server_request_duration_seconds{method,route,code}  Duration of HTTP requests in seconds.
//
// method is the request's method ("_OTHER" if it's unknown), route the pattern of the route that matched (empty if
// none did, so that unmatched paths don't each make a series), and code the response's status. A request's duration
// is until the handler returns (before the response is flushed), and the histogram's count is the number of requests.
//
// Copies share their metrics, and the handler.
class http_metrics
{
public:
    explicit http_metrics(http::request_handler next, http_metrics_options options = {});
    explicit http_metrics(http::router&& next, http_metrics_options options = {});

    coro::task<void> operator()(http::server_request& req, http::response_writer& resp) const;

private:
    struct state;
    std::shared_ptr<state> shared;
};

// span_handler is passed each finished span: e.g. to batch them up and export them (see
// opentelemetry::export_trace_service_request). It's called on whichever thread finished the request, and mustn't
// throw.
using span_handler = std::function<void(opentelemetry::span&&)>;

// http_tracing makes a server span for each request its handler handles: the child of the span in the request's trace
// context (its traceparent and tracestate headers, see opentelemetry::tracecontext), or else the root of a new trace,
// which is sampled. Every finished span is passed to on_end, sampled or not (its flags say).
//
// Each span is named "{method} {route}" (or "{method}", if no route matched), and has OpenTelemetry's HTTP server
// attributes: http.request.method, http.route, url.path, url.scheme, network.protocol.version and
// http.response.status_code. Its status is an error if the response is a 5xx, or if the handler threw (with the
// exception's message).
//
// NOTE: the handler can't get the span yet, so it can't propagate the trace to the requests it makes.
//
// Copies share the handler and on_end.
class http_tracing
{
public:
    http_tracing(http::request_handler next, span_handler on_end);
    http_tracing(http::router&& next, span_handler on_end);

    coro::task<void> operator()(http::server_request& req, http::response_writer& resp) const;

private:
    struct state;
    std::shared_ptr<const state> shared;
};

}
