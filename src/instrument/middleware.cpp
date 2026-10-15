#include "instrument/middleware.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "instrument/opentelemetry/protocol.hpp"
#include "instrument/opentelemetry/tracecontext.hpp"
#include "instrument/prometheus/gauge.hpp"
#include "instrument/prometheus/histogram.hpp"
#include "instrument/prometheus/registry.hpp"

namespace
{

using namespace std::string_view_literals;

namespace http          = net::http;
namespace opentelemetry = net::instrument::opentelemetry;

// responded_status is the status the server sends: what the handler sent, if it did, otherwise 500 Internal Server
// Error if it threw, or 200 OK if it didn't.
http::status responded_status(const http::response_writer& resp, bool threw) noexcept
{
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

    http::request_handler next;
    std::vector<double>   buckets;
    prometheus::gauge&    active;

    // NOTE: a cache of the histogram's series, so that most requests don't need the registry's (exclusive) lock.
    std::shared_mutex                                     mutex;
    std::map<series, prometheus::histogram*, std::less<>> durations;

    state(http::request_handler&& next, std::vector<double>&& buckets)
        : next{std::move(next)}
        , buckets{std::move(buckets)}
        , active{prometheus::registry::register_metric(
              prometheus::gauge{"http_server_active_requests", "Number of HTTP requests being handled."})}
    {}

    prometheus::histogram& duration(http::request_method method, http::status code, std::string_view route)
    {
        const auto key = std::tuple{method, code, route};

        {
            std::shared_lock lock{mutex};
            if (auto iter = durations.find(key); iter != durations.end()) return *iter->second;
        }

        std::lock_guard lock{mutex};
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

    static coro::task<void> handle(std::shared_ptr<state> self, http::server_request& req, http::response_writer& resp)
    {
        self->active.increment();
        const auto start = std::chrono::steady_clock::now();

        // NOTE: a co_await can't be in a catch block, so a failure is rethrown after it.
        std::exception_ptr failure;
        try
        {
            co_await self->next(req, resp);
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        self->active.decrement();
        self->duration(req.method, responded_status(resp, failure != nullptr), req.route).observe(elapsed.count());

        if (failure != nullptr) std::rethrow_exception(failure);
    }
};

http_metrics::http_metrics(http::request_handler next, http_metrics_options options)
    : shared{std::make_shared<state>(std::move(next), std::move(options.buckets))}
{}

http_metrics::http_metrics(http::router&& next, http_metrics_options options)
    : http_metrics{http::make_handler(std::move(next)), std::move(options)}
{}

coro::task<void> http_metrics::operator()(http::server_request& req, http::response_writer& resp) const
{
    return state::handle(shared, req, resp);
}

struct http_tracing::state
{
    http::request_handler next;
    span_handler          on_end;

    static coro::task<void>
    handle(std::shared_ptr<const state> self, http::server_request& req, http::response_writer& resp)
    {
        auto parsed = opentelemetry::tracecontext::parse(req.headers);

        opentelemetry::span span;
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

        std::exception_ptr failure;
        try
        {
            co_await self->next(req, resp);
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        span.end_time = std::chrono::system_clock::now();

        const auto method = method_name(req.method);
        const auto code   = responded_status(resp, failure != nullptr);

        span.name = req.route.empty() ? std::string{method} : std::string{method} + ' ' + req.route;

        // version 2 and later have no minor version.
        auto version = std::to_string(req.version.major);
        if (req.version.major < 2) version += '.' + std::to_string(req.version.minor);

        span.attributes.push_back(attribute("http.request.method", std::string{method}));
        if (!req.route.empty()) span.attributes.push_back(attribute("http.route", req.route));
        span.attributes.push_back(attribute("url.path", req.target.path));
        span.attributes.push_back(attribute("url.scheme", std::string{"http"}));
        span.attributes.push_back(attribute("network.protocol.version", std::move(version)));
        span.attributes.push_back(
            attribute("http.response.status_code", static_cast<std::int64_t>(std::to_underlying(code))));

        // for a server, only a 5xx is an error: a 4xx is the client's.
        if (failure != nullptr)
        {
            span.status = {.message = exception_message(failure), .code = opentelemetry::status_code::error};
        }
        else if (http::status_is_server_error(code))
        {
            span.status = {.message = "", .code = opentelemetry::status_code::error};
        }

        self->on_end(std::move(span));

        if (failure != nullptr) std::rethrow_exception(failure);
    }
};

http_tracing::http_tracing(http::request_handler next, span_handler on_end)
    : shared{std::make_shared<const state>(state{.next = std::move(next), .on_end = std::move(on_end)})}
{}

http_tracing::http_tracing(http::router&& next, span_handler on_end)
    : http_tracing{http::make_handler(std::move(next)), std::move(on_end)}
{}

coro::task<void> http_tracing::operator()(http::server_request& req, http::response_writer& resp) const
{
    return state::handle(shared, req, resp);
}

}
