#include "instrument/middleware.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "http/http.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "instrument/opentelemetry/protocol.hpp"
#include "instrument/opentelemetry/tracecontext.hpp"
#include "instrument/prometheus/gauge.hpp"
#include "instrument/prometheus/histogram.hpp"
#include "instrument/prometheus/metric.hpp"
#include "instrument/prometheus/registry.hpp"
#include "util/type_map.hpp"

namespace
{

using namespace std::string_view_literals;

namespace http          = net::http;
namespace opentelemetry = net::instrument::opentelemetry;

// canceled_status is the status recorded for a request that was stopped (see http::server_request::stop) before its
// final response was sent (when the server sends none the handler didn't, nor a 500 for one that threw): 503 Service
// Unavailable if the server was aborted (server_request::aborted), or else 499 Client Closed Request (as nginx logs
// it), the client's side having stopped it. NONE if it wasn't canceled.
http::status canceled_status(const http::server_request& req, const http::response_writer& resp) noexcept
{
    if (resp.sent() || !req.stop.stop_requested()) return http::status::NONE;
    return req.aborted.stop_requested() ? http::status::SERVICE_UNAVAILABLE : http::status::CLIENT_CLOSED_REQUEST;
}

// responded_status is the status the server sends: what the handler sent, if it did, otherwise 500 Internal Server
// Error if it threw, or 200 OK if it didn't; or, if it was canceled, its canceled_status.
http::status responded_status(http::status canceled, const http::response_writer& resp, bool threw) noexcept
{
    if (canceled != http::status::NONE) return canceled;
    if (auto sent = resp.status_code(); sent != http::status::NONE) return sent;
    return threw ? http::status::INTERNAL_SERVER_ERROR : http::status::OK;
}

// method_name is the request's method, as OpenTelemetry's semantic conventions name it: "_OTHER" if it's unknown.
std::string_view method_name(http::request_method method) noexcept
{
    return method == http::request_method::NONE ? "_OTHER"sv : http::method_string(method);
}

// exception_message is the exception's what(), if it has one.
std::string exception_message(const std::exception_ptr& ex)
{
    try
    {
        std::rethrow_exception(ex);
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
    catch (...)
    {
        return "unknown exception";
    }
}

opentelemetry::key_value attribute(std::string key, opentelemetry::any_value::value_t value)
{
    return {.key = std::move(key), .value = {.value = std::move(value)}};
}

}

namespace net::instrument
{

struct http_metrics::state
{
    // the series of the duration histogram: method, status, route.
    using series = std::tuple<http::request_method, http::status, std::string>;

    std::vector<double> buckets;
    prometheus::gauge&  active;

    // NOTE: a cache of the histogram's series, so that most requests don't need the registry's (exclusive) lock.
    std::shared_mutex                                     mutex;
    std::map<series, prometheus::histogram*, std::less<>> durations;

    explicit state(std::vector<double>&& buckets)
        : buckets{std::move(buckets)}
        , active{
              prometheus::registry::register_metric(
                  prometheus::gauge{"http_server_active_requests", "Number of HTTP requests being handled."}),
          }
    {}

    prometheus::histogram& duration(http::request_method method, http::status code, std::string_view route)
    {
        const auto key = std::tuple{method, code, route};

        {
            std::shared_lock lock{mutex};
            if (auto iter = durations.find(key); iter != durations.end()) return *iter->second;
        }

        std::scoped_lock lock{mutex};
        if (auto iter = durations.find(key); iter != durations.end()) return *iter->second;

        auto& h = prometheus::registry::register_metric(prometheus::histogram{
            "http_server_request_duration_seconds",
            "Duration of HTTP requests in seconds.",
            {
              {.key = "code", .val = std::to_string(std::to_underlying(code))},
              {.key = "method", .val = std::string{method_name(method)}},
              {.key = "route", .val = std::string{route}},
              },
            std::vector{buckets},
        });

        durations.emplace(series{method, code, std::string{route}}, &h);
        return h;
    }
};

http_metrics::http_metrics(http_metrics_options options)
    : shared{std::make_shared<state>(std::move(options.buckets))}
{}

std::chrono::steady_clock::time_point http_metrics::begin() const
{
    shared->active.increment();
    return std::chrono::steady_clock::now();
}

void http_metrics::end(std::chrono::steady_clock::time_point start,
                       const http::server_request&           req,
                       const http::response_writer&          resp,
                       bool                                  threw) const
{
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    shared->active.decrement();
    const auto code = responded_status(canceled_status(req, resp), resp, threw);
    shared->duration(req.method, code, req.route).observe(elapsed.count());
}

opentelemetry::span trace::start_child(std::string name, opentelemetry::span_kind kind) const
{
    opentelemetry::span child;
    child.trace_id       = server_span->trace_id;
    child.parent_span_id = server_span->span_id;
    child.span_id        = opentelemetry::tracecontext::new_span_id();
    child.trace_state    = server_span->trace_state;
    child.flags          = server_span->flags;
    child.name           = std::move(name);
    child.kind           = kind;
    child.start_time     = std::chrono::system_clock::now();
    return child;
}

void trace::end(opentelemetry::span&& child) const
{
    if (child.end_time == opentelemetry::time_point{}) child.end_time = std::chrono::system_clock::now();
    (*on_end)(std::move(child));
}

http_tracing::http_tracing(span_handler on_end)
    : on_end{std::make_shared<const span_handler>(std::move(on_end))}
{}

void http_tracing::begin(http::server_request& req, opentelemetry::span& span) const
{
    auto parsed = opentelemetry::tracecontext::parse(req.headers);
    if (parsed.has_value())
    {
        span = std::move(*parsed);
    }
    else
    {
        span.trace_id = opentelemetry::tracecontext::new_trace_id();
        span.span_id  = opentelemetry::tracecontext::new_span_id();
        span.flags    = opentelemetry::span_flags::sampled | opentelemetry::span_flags::random;
    }

    span.kind       = opentelemetry::span_kind::server;
    span.start_time = std::chrono::system_clock::now();

    req.extensions.emplace<trace>(&span, on_end.get());
}

void http_tracing::end(http::server_request&        req,
                       const http::response_writer& resp,
                       opentelemetry::span&&        span,
                       const std::exception_ptr&    failure) const
{
    req.extensions.erase<trace>();

    span.end_time = std::chrono::system_clock::now();

    const auto method   = method_name(req.method);
    const auto canceled = canceled_status(req, resp);
    const auto code     = responded_status(canceled, resp, failure != nullptr);

    // the client's side stopped it.
    const bool client_closed = canceled == http::status::CLIENT_CLOSED_REQUEST;

    span.name = std::string{method};
    if (!req.route.empty()) span.name.append(1, ' ').append(req.route);

    // version 2 and later have no minor version.
    auto version = std::to_string(req.version.major);
    if (req.version.major < 2) version += '.' + std::to_string(req.version.minor);

    span.attributes.push_back(attribute("http.request.method", std::string{method}));
    if (!req.route.empty()) span.attributes.push_back(attribute("http.route", std::string{req.route}));
    span.attributes.push_back(attribute("url.path", req.target.path));
    span.attributes.push_back(attribute("url.scheme", std::string{"http"}));
    span.attributes.push_back(attribute("network.protocol.version", std::move(version)));
    span.attributes.push_back(
        attribute("http.response.status_code", static_cast<std::int64_t>(std::to_underlying(code))));

    // for a server, only a 5xx is an error: a 4xx is the client's, as is a request its side canceled, whatever the
    // handler threw for it (one the server's abort canceled is a 503, an error as any).
    if (failure != nullptr && !client_closed)
    {
        span.status = {.message = exception_message(failure), .code = opentelemetry::status_code::error};
    }
    else if (http::status_is_server_error(code))
    {
        span.status = {.message = "", .code = opentelemetry::status_code::error};
    }

    (*on_end)(std::move(span));
}

}
