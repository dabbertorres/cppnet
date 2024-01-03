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

// clang-format off
using any_value_t = std::variant<
    std::string,
    bool,
    std::int64_t,
    double,
    array_value,
    key_value_list,
    std::vector<std::byte>
>;
// clang-format on

struct array_value
{
    std::vector<any_value> values;
};

void to_json(nlohmann::json& j, const array_value& value);

struct any_value
{
    any_value_t value;
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

using trace_id = std::array<std::byte, 16>;
using span_id  = std::array<std::byte, 8>;

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
    time_point             time_unix_nano;
    std::string            name;
    std::vector<key_value> attributes;
    std::uint32_t          dropped_attributes_count;
};

void to_json(nlohmann::json& j, const event& value);

struct link
{
    trace_id               trace_id;
    span_id                span_id;
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
    std::byte              version;
    trace_id               trace_id;
    std::optional<span_id> parent_span_id;
    span_id                span_id;
    std::string            trace_state;
    span_flags             flags;
    std::string            name;
    span_kind              kind;
    time_point             start_time_unix_nano;
    time_point             end_time_unix_nano;
    std::vector<key_value> attributes;
    std::uint32_t          dropped_attributes_count;
    std::vector<event>     events;
    std::uint32_t          dropped_events_count;
    std::vector<link>      links;
    std::uint32_t          dropped_links_count;
    status                 status;
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
    resource                 resource;
    std::vector<scope_spans> scope_spans;
    std::string              schema_url;
};

void to_json(nlohmann::json& j, const resource_spans& value);

struct export_trace_service_request
{
    std::vector<resource_spans> resource_spans;
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

}
