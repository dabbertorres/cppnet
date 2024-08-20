#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace net::instrument::opentelemetry
{

struct key_value;
struct array_value;
struct any_value;

struct key_value_list
{
    std::vector<key_value> values;
};

void to_json(nlohmann::json& j, const key_value_list& value);

struct array_value
{
    std::vector<any_value> values;
};

void to_json(nlohmann::json& j, const array_value& value);

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

void to_json(nlohmann::json& j, const any_value& value);

struct key_value
{
    std::string key;
    any_value   value;
};

void to_json(nlohmann::json& j, const key_value& value);

struct resource
{
    std::vector<key_value> attributes;
    std::uint32_t          dropped_attributes_count;
};

void to_json(nlohmann::json& j, const resource& value);

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

enum class span_kind
{
    unspecified = 0,
    internal    = 1,
    server      = 2,
    client      = 3,
    producer    = 4,
    consumer    = 5,
};

using time_point = std::chrono::time_point<std::chrono::steady_clock, std::chrono::nanoseconds>;

struct event
{
    time_point             time;
    std::string            name;
    std::vector<key_value> attributes;
    std::uint32_t          dropped_attributes_count;
};

void to_json(nlohmann::json& j, const event& value);

struct link
{
    trace_id_t             trace_id;
    span_id_t              span_id;
    std::string            trace_state;
    std::vector<key_value> attributes;
    std::uint32_t          dropped_attributes_count;
    span_flags             flags;
};

void to_json(nlohmann::json& j, const link& value);

enum class status_code
{
    unset = 0,
    ok    = 1,
    error = 2,
};

struct status
{
    std::string message;
    status_code code;
};

void to_json(nlohmann::json& j, const status& value);

struct span
{
    std::byte                version;
    trace_id_t               trace_id;
    std::optional<span_id_t> parent_span_id;
    span_id_t                span_id;
    std::string              trace_state;
    span_flags               flags;
    std::string              name;
    span_kind                kind;
    time_point               start_time;
    time_point               end_time;
    std::vector<key_value>   attributes;
    std::uint32_t            dropped_attributes_count;
    std::vector<event>       events;
    std::uint32_t            dropped_events_count;
    std::vector<link>        links;
    std::uint32_t            dropped_links_count;
    struct status            status;
};

void to_json(nlohmann::json& j, const span& value);

struct instrumentation_scope
{
    std::string            name;
    std::string            version;
    std::vector<key_value> attributes;
    std::uint32_t          dropped_attributes_count;
};

void to_json(nlohmann::json& j, const instrumentation_scope& value);

struct scope_spans
{
    instrumentation_scope scope;
    std::vector<span>     spans;
    std::string           schema_url;
};

void to_json(nlohmann::json& j, const scope_spans& value);

struct resource_spans
{
    struct resource                 resource;
    std::vector<struct scope_spans> scope_spans;
    std::string                     schema_url;
};

void to_json(nlohmann::json& j, const resource_spans& value);

enum class data_point_flags : std::uint8_t
{
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
    span_id_t                          span_id;
    trace_id_t                         trace_id;
};

void to_json(nlohmann::json& j, const exemplar& value);

struct number_data_point
{
    std::vector<key_value>             attributes;
    time_point                         start_time;
    time_point                         time;
    std::variant<double, std::int64_t> value;
    std::vector<exemplar>              exemplars;
    data_point_flags                   flags;
};

void to_json(nlohmann::json& j, const number_data_point& value);

struct gauge
{
    std::vector<number_data_point> data_points;
};

void to_json(nlohmann::json& j, const gauge& value);

enum class aggregation_temporality : std::uint8_t
{
    delta      = 1,
    cumulative = 2,
};

struct sum
{
    std::vector<number_data_point> data_points;
    enum aggregation_temporality   aggregation_temporality;
    bool                           is_monotonic;
};

void to_json(nlohmann::json& j, const sum& value);

struct histogram
{
    struct data_point
    {
        std::vector<key_value>     attributes;
        time_point                 start_time;
        time_point                 time;
        std::uint64_t              count;
        std::optional<double>      sum;
        std::vector<std::uint64_t> bucket_counts;
        std::vector<double>        explicit_bounds;
        std::vector<exemplar>      exemplars;
        data_point_flags           flags;
        std::optional<double>      min;
        std::optional<double>      max;
    };

    std::vector<data_point>      data_points;
    enum aggregation_temporality aggregation_temporality;
};

void to_json(nlohmann::json& j, const histogram& value);

struct exponential_histogram
{
    struct data_point
    {
        struct buckets
        {
            std::int32_t               offset;
            std::vector<std::uint64_t> bucket_counts;
        };

        std::vector<key_value> attributes;
        time_point             start_time;
        time_point             time;
        std::uint64_t          count;
        std::optional<double>  sum;
        std::int32_t           scale;
        std::uint64_t          zero_count;
        buckets                positive;
        buckets                negative;
        data_point_flags       flags;
        std::vector<exemplar>  exemplars;
        std::optional<double>  min;
        std::optional<double>  max;
        double                 zero_threshold;
    };

    std::vector<data_point>      data_points;
    enum aggregation_temporality aggregation_temporality;
};

void to_json(nlohmann::json& j, const exponential_histogram& value);

struct summary
{
    struct data_point
    {
        struct value_at_quantile
        {
            double quantile;
            double value;
        };

        std::vector<key_value>         attributes;
        time_point                     start_time;
        time_point                     time;
        std::uint64_t                  count;
        double                         sum;
        std::vector<value_at_quantile> quantile_values;
        data_point_flags               flags;
    };

    std::vector<data_point> data_points;
};

void to_json(nlohmann::json& j, const summary& value);

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

void to_json(nlohmann::json& j, const metric& value);

struct scope_metrics
{
    instrumentation_scope scope;
    std::vector<metric>   metrics;
    std::string           schema_url;
};

void to_json(nlohmann::json& j, const scope_metrics& value);

struct resource_metrics
{
    struct resource                   resource;
    std::vector<struct scope_metrics> scope_metrics;
    std::string                       schema_url;
};

void to_json(nlohmann::json& j, const resource_metrics& value);

struct export_trace_service_request
{
    std::vector<struct resource_spans> resource_spans;
};

void to_json(nlohmann::json& j, const export_trace_service_request& value);

struct export_trace_partial_success
{
    std::int64_t rejected_spans;
    std::string  error_message;
};

void from_json(const nlohmann::json& j, export_trace_partial_success& value);

struct export_trace_service_response
{
    std::optional<export_trace_partial_success> partial_success;
};

void from_json(const nlohmann::json& j, export_trace_service_response& value);

struct export_metrics_service_request
{
    std::vector<struct resource_metrics> resource_metrics;
};

void to_json(nlohmann::json& j, const export_metrics_service_request& value);

struct export_metrics_partial_success
{
    std::int64_t rejected_data_points;
    std::string  error_message;
};

void from_json(const nlohmann::json& j, export_metrics_partial_success& value);

struct export_metrics_service_response
{
    std::optional<export_metrics_partial_success> partial_success;
};

void from_json(const nlohmann::json& j, export_metrics_service_response& value);

}
