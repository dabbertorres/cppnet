#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>

// These are OTLP's types, which to_json writes as OTLP/JSON (proto3's JSON mapping, e.g. "traceId" as hex), and
// from_json reads the export services' responses from.
namespace net::instrument::opentelemetry
{

// NOTE: these types refer to each other.
struct key_value;   // IWYU pragma: keep
struct array_value; // IWYU pragma: keep
struct any_value;   // IWYU pragma: keep

struct key_value_list
{
    std::vector<key_value> values;
};

[[nodiscard]] std::string to_json(const key_value_list& value);

struct array_value
{
    std::vector<any_value> values;
};

[[nodiscard]] std::string to_json(const array_value& value);

struct any_value
{
    // clang-format off
    using value_t = std::variant<
        std::string,
        bool,
        std::int64_t,
        double,
        array_value,
        key_value_list,
        std::vector<std::byte>
    >;
    // clang-format on

    value_t value;
};

[[nodiscard]] std::string to_json(const any_value& value);

struct key_value
{
    std::string key;
    any_value   value;
};

[[nodiscard]] std::string to_json(const key_value& value);

struct resource
{
    std::vector<key_value> attributes;
    std::uint32_t          dropped_attributes_count{};
};

[[nodiscard]] std::string to_json(const resource& value);

using trace_id_t = std::array<std::byte, 16>;
using span_id_t  = std::array<std::byte, 8>;

enum class span_flags : std::uint8_t
{
    zero    = 0,
    sampled = 1,
    random  = 2,

    w3c_mask = 0xff,
};

constexpr span_flags operator&(span_flags lhs, span_flags rhs) noexcept
{
    using ut = std::underlying_type_t<span_flags>;

    return static_cast<span_flags>(static_cast<ut>(lhs) & static_cast<ut>(rhs));
}

constexpr span_flags operator|(span_flags lhs, span_flags rhs) noexcept
{
    using ut = std::underlying_type_t<span_flags>;

    return static_cast<span_flags>(static_cast<ut>(lhs) | static_cast<ut>(rhs));
}

constexpr span_flags& operator|=(span_flags& lhs, span_flags rhs) noexcept
{
    lhs = lhs | rhs;
    return lhs;
}

enum class span_kind : std::uint8_t
{
    unspecified = 0,
    internal    = 1,
    server      = 2,
    client      = 3,
    producer    = 4,
    consumer    = 5,
};

// NOTE: system_clock's, as OTLP's times are since the Unix epoch.
using time_point = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;

struct event
{
    time_point             time;
    std::string            name;
    std::vector<key_value> attributes;
    std::uint32_t          dropped_attributes_count{};
};

[[nodiscard]] std::string to_json(const event& value);

struct link
{
    trace_id_t             trace_id{};
    span_id_t              span_id{};
    std::string            trace_state;
    std::vector<key_value> attributes;
    std::uint32_t          dropped_attributes_count{};
    span_flags             flags{};
};

[[nodiscard]] std::string to_json(const link& value);

enum class status_code : std::uint8_t
{
    unset = 0,
    ok    = 1,
    error = 2,
};

struct status
{
    std::string message;
    status_code code{};
};

[[nodiscard]] std::string to_json(const status& value);

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): its fields are in the order of OTLP's Span message.
struct span
{
    std::byte                version{};
    trace_id_t               trace_id{};
    std::optional<span_id_t> parent_span_id;
    span_id_t                span_id{};
    std::string              trace_state;
    span_flags               flags{};
    std::string              name;
    span_kind                kind{};
    time_point               start_time;
    time_point               end_time;
    std::vector<key_value>   attributes;
    std::uint32_t            dropped_attributes_count{};
    std::vector<event>       events;
    std::uint32_t            dropped_events_count{};
    std::vector<link>        links;
    std::uint32_t            dropped_links_count{};
    struct status            status;
};

[[nodiscard]] std::string to_json(const span& value);

struct instrumentation_scope
{
    std::string            name;
    std::string            version;
    std::vector<key_value> attributes;
    std::uint32_t          dropped_attributes_count{};
};

[[nodiscard]] std::string to_json(const instrumentation_scope& value);

struct scope_spans
{
    instrumentation_scope scope;
    std::vector<span>     spans;
    std::string           schema_url;
};

[[nodiscard]] std::string to_json(const scope_spans& value);

struct resource_spans
{
    struct resource                 resource;
    std::vector<struct scope_spans> scope_spans;
    std::string                     schema_url;
};

[[nodiscard]] std::string to_json(const resource_spans& value);

enum class data_point_flags : std::uint8_t
{
    none              = 0,
    no_recorded_value = 1,
};

constexpr data_point_flags operator&(data_point_flags lhs, data_point_flags rhs) noexcept
{
    using ut = std::underlying_type_t<data_point_flags>;

    return static_cast<data_point_flags>(static_cast<ut>(lhs) & static_cast<ut>(rhs));
}

constexpr data_point_flags operator|(data_point_flags lhs, data_point_flags rhs) noexcept
{
    using ut = std::underlying_type_t<data_point_flags>;

    return static_cast<data_point_flags>(static_cast<ut>(lhs) | static_cast<ut>(rhs));
}

constexpr data_point_flags& operator|=(data_point_flags& lhs, data_point_flags rhs) noexcept
{
    lhs = lhs | rhs;
    return lhs;
}

struct exemplar
{
    std::vector<key_value>             filtered_attributes;
    time_point                         time;
    std::variant<double, std::int64_t> value;
    span_id_t                          span_id{};
    trace_id_t                         trace_id{};
};

[[nodiscard]] std::string to_json(const exemplar& value);

struct number_data_point
{
    std::vector<key_value>             attributes;
    time_point                         start_time;
    time_point                         time;
    std::variant<double, std::int64_t> value;
    std::vector<exemplar>              exemplars;
    data_point_flags                   flags{};
};

[[nodiscard]] std::string to_json(const number_data_point& value);

struct gauge
{
    std::vector<number_data_point> data_points;
};

[[nodiscard]] std::string to_json(const gauge& value);

enum class aggregation_temporality : std::uint8_t
{
    unspecified = 0,
    delta       = 1,
    cumulative  = 2,
};

struct sum
{
    std::vector<number_data_point> data_points;
    enum aggregation_temporality aggregation_temporality
    {
    };
    bool is_monotonic{};
};

[[nodiscard]] std::string to_json(const sum& value);

struct histogram
{
    struct data_point
    {
        std::vector<key_value>     attributes;
        time_point                 start_time;
        time_point                 time;
        std::uint64_t              count{};
        std::optional<double>      sum;
        std::vector<std::uint64_t> bucket_counts;
        std::vector<double>        explicit_bounds;
        std::vector<exemplar>      exemplars;
        data_point_flags           flags{};
        std::optional<double>      min;
        std::optional<double>      max;
    };

    std::vector<data_point> data_points;
    enum aggregation_temporality aggregation_temporality
    {
    };
};

[[nodiscard]] std::string to_json(const histogram::data_point& value);
[[nodiscard]] std::string to_json(const histogram& value);

struct exponential_histogram
{
    struct data_point
    {
        struct buckets
        {
            std::int32_t               offset{};
            std::vector<std::uint64_t> bucket_counts;
        };

        std::vector<key_value> attributes;
        time_point             start_time;
        time_point             time;
        std::uint64_t          count{};
        std::optional<double>  sum;
        std::int32_t           scale{};
        std::uint64_t          zero_count{};
        buckets                positive;
        buckets                negative;
        data_point_flags       flags{};
        std::vector<exemplar>  exemplars;
        std::optional<double>  min;
        std::optional<double>  max;
        double                 zero_threshold{};
    };

    std::vector<data_point> data_points;
    enum aggregation_temporality aggregation_temporality
    {
    };
};

[[nodiscard]] std::string to_json(const exponential_histogram::data_point::buckets& value);
[[nodiscard]] std::string to_json(const exponential_histogram::data_point& value);
[[nodiscard]] std::string to_json(const exponential_histogram& value);

struct summary
{
    struct data_point
    {
        struct value_at_quantile
        {
            double quantile{};
            double value{};
        };

        std::vector<key_value>         attributes;
        time_point                     start_time;
        time_point                     time;
        std::uint64_t                  count{};
        double                         sum{};
        std::vector<value_at_quantile> quantile_values;
        data_point_flags               flags{};
    };

    std::vector<data_point> data_points;
};

[[nodiscard]] std::string to_json(const summary::data_point::value_at_quantile& value);
[[nodiscard]] std::string to_json(const summary::data_point& value);
[[nodiscard]] std::string to_json(const summary& value);

struct metric
{
    // clang-format off
    using data_t = std::variant<
        gauge,
        sum,
        histogram,
        exponential_histogram,
        summary
    >;
    // clang-format on

    std::string            name;
    std::string            description;
    std::string            unit;
    data_t                 data;
    std::vector<key_value> metadata;
};

[[nodiscard]] std::string to_json(const metric& value);

struct scope_metrics
{
    instrumentation_scope scope;
    std::vector<metric>   metrics;
    std::string           schema_url;
};

[[nodiscard]] std::string to_json(const scope_metrics& value);

struct resource_metrics
{
    struct resource                   resource;
    std::vector<struct scope_metrics> scope_metrics;
    std::string                       schema_url;
};

[[nodiscard]] std::string to_json(const resource_metrics& value);

struct export_trace_service_request
{
    std::vector<struct resource_spans> resource_spans;
};

[[nodiscard]] std::string to_json(const export_trace_service_request& value);

struct export_trace_partial_success
{
    std::int64_t rejected_spans{};
    std::string  error_message;
};

struct export_trace_service_response
{
    std::optional<export_trace_partial_success> partial_success;
};

struct export_metrics_service_request
{
    std::vector<struct resource_metrics> resource_metrics;
};

[[nodiscard]] std::string to_json(const export_metrics_service_request& value);

struct export_metrics_partial_success
{
    std::int64_t rejected_data_points{};
    std::string  error_message;
};

struct export_metrics_service_response
{
    std::optional<export_metrics_partial_success> partial_success;
};

// from_json reads an export service's response (export_trace_service_response or export_metrics_service_response),
// failing with std::errc::invalid_argument if it isn't one.
template<typename T>
[[nodiscard]] std::expected<T, std::error_code> from_json(std::string_view json);

template<>
std::expected<export_trace_service_response, std::error_code>
from_json<export_trace_service_response>(std::string_view json);

template<>
std::expected<export_metrics_service_response, std::error_code>
from_json<export_metrics_service_response>(std::string_view json);
}
