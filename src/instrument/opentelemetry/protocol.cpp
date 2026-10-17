#include "instrument/opentelemetry/protocol.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <glaze/glaze.hpp>

#include "encoding/base64.hpp"
#include "encoding/hex.hpp"

namespace otel = net::instrument::opentelemetry;

namespace
{

// json_double is a double as proto3's JSON mapping writes it: a number, or (as JSON numbers can't be) "NaN",
// "Infinity" or "-Infinity".
using json_double = std::variant<double, std::string_view>;

json_double to_json_double(double v)
{
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    return v;
}

std::optional<json_double> to_json_double(const std::optional<double>& v)
{
    if (!v.has_value()) return std::nullopt;
    return to_json_double(*v);
}

std::vector<json_double> to_json_doubles(const std::vector<double>& values)
{
    std::vector<json_double> out;
    out.reserve(values.size());
    for (double v : values) out.push_back(to_json_double(v));
    return out;
}

std::int64_t unix_nanos(otel::time_point t) { return t.time_since_epoch().count(); }

// number is an enum as OTLP/JSON writes it: its number, rather than its name.
template<typename E>
int number(E e)
{
    return static_cast<int>(std::to_underlying(e));
}

template<typename Id>
std::string hex(const Id& id)
{
    return net::encoding::hex::encode(id);
}

// int64_field is an int64 as proto3's JSON mapping reads it: usually a string, but possibly a number.
using int64_field = std::optional<std::variant<std::int64_t, std::string>>;

std::expected<std::int64_t, std::error_code> int64_from(const int64_field& field)
{
    if (!field.has_value()) return 0;
    if (const auto* number = std::get_if<std::int64_t>(&*field)) return *number;

    const auto& text = std::get<std::string>(*field);

    std::int64_t v  = 0;
    auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (err != std::errc{} || end != text.data() + text.size())
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    return v;
}

// The export services' responses, as they're read, before their int64s are checked.
struct trace_partial_success_json
{
    int64_field                rejected_spans;
    std::optional<std::string> error_message;
};

struct trace_response_json
{
    std::optional<trace_partial_success_json> partial_success;
};

struct metrics_partial_success_json
{
    int64_field                rejected_data_points;
    std::optional<std::string> error_message;
};

struct metrics_response_json
{
    std::optional<metrics_partial_success_json> partial_success;
};
}

// How each type is written (or read): OTLP's names, and proto3's JSON mapping of its values. Fields that are null (an
// empty optional, or a pointer to another alternative of a variant) are left out.
// NOLINTBEGIN(readability-identifier-naming): glaze's names.
template<>
struct glz::meta<otel::key_value_list>
{
    static constexpr auto value = glz::object("values", &otel::key_value_list::values);
};

template<>
struct glz::meta<otel::array_value>
{
    static constexpr auto value = glz::object("values", &otel::array_value::values);
};

template<>
struct glz::meta<otel::any_value>
{
    using T = otel::any_value;

    // NOTE: one of them, for the value's type.
    static constexpr auto value = glz::object(
        "stringValue",
        [](const T& v) { return std::get_if<std::string>(&v.value); },
        "boolValue",
        [](const T& v) { return std::get_if<bool>(&v.value); },
        "intValue",
        [](const T& v) { return std::get_if<std::int64_t>(&v.value); },
        "doubleValue",
        [](const T& v) -> std::optional<json_double>
        {
            if (const auto* d = std::get_if<double>(&v.value)) return ::to_json_double(*d);
            return std::nullopt;
        },
        "arrayValue",
        [](const T& v) { return std::get_if<otel::array_value>(&v.value); },
        "kvlistValue",
        [](const T& v) { return std::get_if<otel::key_value_list>(&v.value); },
        "bytesValue",
        [](const T& v) -> std::optional<std::string>
        {
            const auto* bytes = std::get_if<std::vector<std::byte>>(&v.value);
            if (bytes == nullptr) return std::nullopt;
            return net::encoding::base64::encoding{}.encode(*bytes);
        });
};

template<>
struct glz::meta<otel::key_value>
{
    static constexpr auto value = glz::object("key", &otel::key_value::key, "value", &otel::key_value::value);
};

template<>
struct glz::meta<otel::resource>
{
    static constexpr auto value = glz::object(
        "attributes", &otel::resource::attributes, "droppedAttributesCount", &otel::resource::dropped_attributes_count);
};

template<>
struct glz::meta<otel::event>
{
    using T = otel::event;

    static constexpr auto value = glz::object(
        "timeUnixNano",
        [](const T& e) { return ::unix_nanos(e.time); },
        "name",
        &T::name,
        "attributes",
        &T::attributes,
        "droppedAttributesCount",
        &T::dropped_attributes_count);
};

template<>
struct glz::meta<otel::link>
{
    using T = otel::link;

    static constexpr auto value = glz::object(
        "traceId",
        [](const T& l) { return ::hex(l.trace_id); },
        "spanId",
        [](const T& l) { return ::hex(l.span_id); },
        "traceState",
        &T::trace_state,
        "attributes",
        &T::attributes,
        "droppedAttributesCount",
        &T::dropped_attributes_count,
        "flags",
        [](const T& l) { return ::number(l.flags); });
};

template<>
struct glz::meta<otel::status>
{
    static constexpr auto value =
        glz::object("message", &otel::status::message, "code", [](const otel::status& s) { return ::number(s.code); });
};

template<>
struct glz::meta<otel::span>
{
    using T = otel::span;

    static constexpr auto value = glz::object(
        "traceId",
        [](const T& s) { return ::hex(s.trace_id); },
        "spanId",
        [](const T& s) { return ::hex(s.span_id); },
        "traceState",
        [](const T& s) -> std::optional<std::string_view>
        {
            if (s.trace_state.empty()) return std::nullopt;
            return s.trace_state;
        },
        "parentSpanId",
        [](const T& s) -> std::optional<std::string>
        {
            if (!s.parent_span_id.has_value()) return std::nullopt;
            return ::hex(*s.parent_span_id);
        },
        "flags",
        [](const T& s) -> std::optional<int>
        {
            if (s.flags == otel::span_flags::zero) return std::nullopt;
            return ::number(s.flags);
        },
        "name",
        &T::name,
        "kind",
        [](const T& s) { return ::number(s.kind); },
        "startTimeUnixNano",
        [](const T& s) { return ::unix_nanos(s.start_time); },
        "endTimeUnixNano",
        [](const T& s) { return ::unix_nanos(s.end_time); },
        "attributes",
        &T::attributes,
        "droppedAttributesCount",
        &T::dropped_attributes_count,
        "events",
        &T::events,
        "droppedEventsCount",
        &T::dropped_events_count,
        "links",
        &T::links,
        "droppedLinksCount",
        &T::dropped_links_count,
        "status",
        &T::status);
};

template<>
struct glz::meta<otel::instrumentation_scope>
{
    using T = otel::instrumentation_scope;

    static constexpr auto value = glz::object("name",
                                              &T::name,
                                              "version",
                                              &T::version,
                                              "attributes",
                                              &T::attributes,
                                              "droppedAttributesCount",
                                              &T::dropped_attributes_count);
};

template<>
struct glz::meta<otel::scope_spans>
{
    using T = otel::scope_spans;

    static constexpr auto value = glz::object("scope", &T::scope, "spans", &T::spans, "schemaUrl", &T::schema_url);
};

template<>
struct glz::meta<otel::resource_spans>
{
    using T = otel::resource_spans;

    static constexpr auto value =
        glz::object("resource", &T::resource, "scopeSpans", &T::scope_spans, "schemaUrl", &T::schema_url);
};

template<>
struct glz::meta<otel::exemplar>
{
    using T = otel::exemplar;

    static constexpr auto value = glz::object(
        "filteredAttributes",
        &T::filtered_attributes,
        "timeUnixNano",
        [](const T& e) { return ::unix_nanos(e.time); },
        "asDouble",
        [](const T& e) -> std::optional<json_double>
        {
            if (const auto* d = std::get_if<double>(&e.value)) return ::to_json_double(*d);
            return std::nullopt;
        },
        "asInt",
        [](const T& e) { return std::get_if<std::int64_t>(&e.value); },
        "spanId",
        [](const T& e) { return ::hex(e.span_id); },
        "traceId",
        [](const T& e) { return ::hex(e.trace_id); });
};

template<>
struct glz::meta<otel::number_data_point>
{
    using T = otel::number_data_point;

    static constexpr auto value = glz::object(
        "attributes",
        &T::attributes,
        "startTimeUnixNano",
        [](const T& p) { return ::unix_nanos(p.start_time); },
        "timeUnixNano",
        [](const T& p) { return ::unix_nanos(p.time); },
        "asDouble",
        [](const T& p) -> std::optional<json_double>
        {
            if (const auto* d = std::get_if<double>(&p.value)) return ::to_json_double(*d);
            return std::nullopt;
        },
        "asInt",
        [](const T& p) { return std::get_if<std::int64_t>(&p.value); },
        "exemplars",
        &T::exemplars,
        "flags",
        [](const T& p) { return ::number(p.flags); });
};

template<>
struct glz::meta<otel::gauge>
{
    static constexpr auto value = glz::object("dataPoints", &otel::gauge::data_points);
};

template<>
struct glz::meta<otel::sum>
{
    using T = otel::sum;

    static constexpr auto value = glz::object(
        "dataPoints",
        &T::data_points,
        "aggregationTemporality",
        [](const T& s) { return ::number(s.aggregation_temporality); },
        "isMonotonic",
        &T::is_monotonic);
};

template<>
struct glz::meta<otel::histogram::data_point>
{
    using T = otel::histogram::data_point;

    static constexpr auto value = glz::object(
        "attributes",
        &T::attributes,
        "startTimeUnixNano",
        [](const T& p) { return ::unix_nanos(p.start_time); },
        "timeUnixNano",
        [](const T& p) { return ::unix_nanos(p.time); },
        "count",
        &T::count,
        "sum",
        [](const T& p) { return ::to_json_double(p.sum); },
        "bucketCounts",
        &T::bucket_counts,
        "explicitBounds",
        [](const T& p) { return ::to_json_doubles(p.explicit_bounds); },
        "exemplars",
        &T::exemplars,
        "flags",
        [](const T& p) { return ::number(p.flags); },
        "min",
        [](const T& p) { return ::to_json_double(p.min); },
        "max",
        [](const T& p) { return ::to_json_double(p.max); });
};

template<>
struct glz::meta<otel::histogram>
{
    using T = otel::histogram;

    static constexpr auto value = glz::object("dataPoints",
                                              &T::data_points,
                                              "aggregationTemporality",
                                              [](const T& h) { return ::number(h.aggregation_temporality); });
};

template<>
struct glz::meta<otel::exponential_histogram::data_point::buckets>
{
    using T = otel::exponential_histogram::data_point::buckets;

    static constexpr auto value = glz::object("offset", &T::offset, "bucketCounts", &T::bucket_counts);
};

template<>
struct glz::meta<otel::exponential_histogram::data_point>
{
    using T = otel::exponential_histogram::data_point;

    static constexpr auto value = glz::object(
        "attributes",
        &T::attributes,
        "startTimeUnixNano",
        [](const T& p) { return ::unix_nanos(p.start_time); },
        "timeUnixNano",
        [](const T& p) { return ::unix_nanos(p.time); },
        "count",
        &T::count,
        "sum",
        [](const T& p) { return ::to_json_double(p.sum); },
        "scale",
        &T::scale,
        "zeroCount",
        &T::zero_count,
        "positive",
        &T::positive,
        "negative",
        &T::negative,
        "flags",
        [](const T& p) { return ::number(p.flags); },
        "exemplars",
        &T::exemplars,
        "min",
        [](const T& p) { return ::to_json_double(p.min); },
        "max",
        [](const T& p) { return ::to_json_double(p.max); },
        "zeroThreshold",
        [](const T& p) { return ::to_json_double(p.zero_threshold); });
};

template<>
struct glz::meta<otel::exponential_histogram>
{
    using T = otel::exponential_histogram;

    static constexpr auto value = glz::object("dataPoints",
                                              &T::data_points,
                                              "aggregationTemporality",
                                              [](const T& h) { return ::number(h.aggregation_temporality); });
};

template<>
struct glz::meta<otel::summary::data_point::value_at_quantile>
{
    using T = otel::summary::data_point::value_at_quantile;

    static constexpr auto value = glz::object(
        "quantile",
        [](const T& q) { return ::to_json_double(q.quantile); },
        "value",
        [](const T& q) { return ::to_json_double(q.value); });
};

template<>
struct glz::meta<otel::summary::data_point>
{
    using T = otel::summary::data_point;

    static constexpr auto value = glz::object(
        "attributes",
        &T::attributes,
        "startTimeUnixNano",
        [](const T& p) { return ::unix_nanos(p.start_time); },
        "timeUnixNano",
        [](const T& p) { return ::unix_nanos(p.time); },
        "count",
        &T::count,
        "sum",
        [](const T& p) { return ::to_json_double(p.sum); },
        "quantileValues",
        &T::quantile_values,
        "flags",
        [](const T& p) { return ::number(p.flags); });
};

template<>
struct glz::meta<otel::summary>
{
    static constexpr auto value = glz::object("dataPoints", &otel::summary::data_points);
};

template<>
struct glz::meta<otel::metric>
{
    using T = otel::metric;

    // NOTE: one of gauge, sum, histogram, exponentialHistogram and summary, for the data's type.
    static constexpr auto value = glz::object(
        "name",
        &T::name,
        "description",
        &T::description,
        "unit",
        &T::unit,
        "gauge",
        [](const T& m) { return std::get_if<otel::gauge>(&m.data); },
        "sum",
        [](const T& m) { return std::get_if<otel::sum>(&m.data); },
        "histogram",
        [](const T& m) { return std::get_if<otel::histogram>(&m.data); },
        "exponentialHistogram",
        [](const T& m) { return std::get_if<otel::exponential_histogram>(&m.data); },
        "summary",
        [](const T& m) { return std::get_if<otel::summary>(&m.data); },
        "metadata",
        &T::metadata);
};

template<>
struct glz::meta<otel::scope_metrics>
{
    using T = otel::scope_metrics;

    static constexpr auto value = glz::object("scope", &T::scope, "metrics", &T::metrics, "schemaUrl", &T::schema_url);
};

template<>
struct glz::meta<otel::resource_metrics>
{
    using T = otel::resource_metrics;

    static constexpr auto value =
        glz::object("resource", &T::resource, "scopeMetrics", &T::scope_metrics, "schemaUrl", &T::schema_url);
};

template<>
struct glz::meta<otel::export_trace_service_request>
{
    static constexpr auto value = glz::object("resourceSpans", &otel::export_trace_service_request::resource_spans);
};

template<>
struct glz::meta<otel::export_metrics_service_request>
{
    static constexpr auto value =
        glz::object("resourceMetrics", &otel::export_metrics_service_request::resource_metrics);
};

template<>
struct glz::meta<trace_partial_success_json>
{
    using T = trace_partial_success_json;

    static constexpr auto value = glz::object("rejectedSpans", &T::rejected_spans, "errorMessage", &T::error_message);
};

template<>
struct glz::meta<trace_response_json>
{
    static constexpr auto value = glz::object("partialSuccess", &trace_response_json::partial_success);
};

template<>
struct glz::meta<metrics_partial_success_json>
{
    using T = metrics_partial_success_json;

    static constexpr auto value =
        glz::object("rejectedDataPoints", &T::rejected_data_points, "errorMessage", &T::error_message);
};

template<>
struct glz::meta<metrics_response_json>
{
    static constexpr auto value = glz::object("partialSuccess", &metrics_response_json::partial_success);
};
// NOLINTEND(readability-identifier-naming)

namespace
{

template<typename T>
std::string write(const T& value)
{
    std::string out;

    // NOTE: writing can only fail for types glaze can't write, which wouldn't compile.
    [[maybe_unused]] auto err = glz::write_json(value, out);
    return out;
}

// read reads json as T, ignoring fields it doesn't know (as proto3's JSON mapping allows).
template<typename T>
std::expected<T, std::error_code> read(std::string_view json)
{
    T value{};
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(value, json))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    return value;
}

}

namespace net::instrument::opentelemetry
{

std::string to_json(const key_value_list& value) { return write(value); }
std::string to_json(const array_value& value) { return write(value); }
std::string to_json(const any_value& value) { return write(value); }
std::string to_json(const key_value& value) { return write(value); }
std::string to_json(const resource& value) { return write(value); }
std::string to_json(const event& value) { return write(value); }
std::string to_json(const link& value) { return write(value); }
std::string to_json(const status& value) { return write(value); }
std::string to_json(const span& value) { return write(value); }
std::string to_json(const instrumentation_scope& value) { return write(value); }
std::string to_json(const scope_spans& value) { return write(value); }
std::string to_json(const resource_spans& value) { return write(value); }
std::string to_json(const exemplar& value) { return write(value); }
std::string to_json(const number_data_point& value) { return write(value); }
std::string to_json(const gauge& value) { return write(value); }
std::string to_json(const sum& value) { return write(value); }
std::string to_json(const histogram::data_point& value) { return write(value); }
std::string to_json(const histogram& value) { return write(value); }
std::string to_json(const exponential_histogram::data_point::buckets& value) { return write(value); }
std::string to_json(const exponential_histogram::data_point& value) { return write(value); }
std::string to_json(const exponential_histogram& value) { return write(value); }
std::string to_json(const summary::data_point::value_at_quantile& value) { return write(value); }
std::string to_json(const summary::data_point& value) { return write(value); }
std::string to_json(const summary& value) { return write(value); }
std::string to_json(const metric& value) { return write(value); }
std::string to_json(const scope_metrics& value) { return write(value); }
std::string to_json(const resource_metrics& value) { return write(value); }
std::string to_json(const export_trace_service_request& value) { return write(value); }
std::string to_json(const export_metrics_service_request& value) { return write(value); }

template<>
std::expected<export_trace_service_response, std::error_code>
from_json<export_trace_service_response>(std::string_view json)
{
    auto read_json = read<trace_response_json>(json);
    if (!read_json.has_value()) return std::unexpected(read_json.error());

    export_trace_service_response out;
    if (const auto& partial = read_json->partial_success; partial.has_value())
    {
        auto rejected = int64_from(partial->rejected_spans);
        if (!rejected.has_value()) return std::unexpected(rejected.error());

        out.partial_success = export_trace_partial_success{
            .rejected_spans = *rejected,
            .error_message  = partial->error_message.value_or(""),
        };
    }

    return out;
}

template<>
std::expected<export_metrics_service_response, std::error_code>
from_json<export_metrics_service_response>(std::string_view json)
{
    auto read_json = read<metrics_response_json>(json);
    if (!read_json.has_value()) return std::unexpected(read_json.error());

    export_metrics_service_response out;
    if (const auto& partial = read_json->partial_success; partial.has_value())
    {
        auto rejected = int64_from(partial->rejected_data_points);
        if (!rejected.has_value()) return std::unexpected(rejected.error());

        out.partial_success = export_metrics_partial_success{
            .rejected_data_points = *rejected,
            .error_message        = partial->error_message.value_or(""),
        };
    }

    return out;
}

}
