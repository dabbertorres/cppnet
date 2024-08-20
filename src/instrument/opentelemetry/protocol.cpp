#include "instrument/opentelemetry/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include "encoding/base64.hpp"
#include "encoding/hex.hpp"
#include "util/overloaded.hpp"

namespace net::instrument::opentelemetry
{

using json = nlohmann::json;

void to_json(nlohmann::json& j, const key_value_list& value) { j["values"] = value.values; }

void to_json(nlohmann::json& j, const array_value& value) { j["values"] = value.values; }

void to_json(json& j, const any_value& value)
{
    std::visit(
        util::overloaded{
            [&](const std::string& s) { j["stringValue"] = s; },
            [&](bool b) { j["boolValue"] = b; },
            [&](std::int64_t i) { j["intValue"] = i; },
            [&](double d) { j["doubleValue"] = d; },
            [&](const array_value& a) { j["arrayValue"]["values"] = a.values; },
            [&](const key_value_list& l) { j["kvlistValue"]["values"] = l.values; },
            [&](const std::vector<std::byte>& b)
            {
                encoding::base64::encoding enc;

                auto encoded    = enc.encode({b.begin(), b.end()});
                j["bytesValue"] = encoded;
            },
        },
        value.value);
}

void to_json(nlohmann::json& j, const key_value& value)
{
    j["key"]   = value.key;
    j["value"] = value.value;
}

void to_json(nlohmann::json& j, const resource& value)
{
    j["attributes"]             = value.attributes;
    j["droppedAttributesCount"] = value.dropped_attributes_count;
}

void to_json(nlohmann::json& j, const event& value)
{
    j["timeUnixNano"]           = value.time.time_since_epoch().count();
    j["name"]                   = value.name;
    j["attributes"]             = value.attributes;
    j["droppedAttributesCount"] = value.dropped_attributes_count;
}

void to_json(nlohmann::json& j, const link& value)
{
    j["traceId"]                = encoding::hex::encode(value.trace_id);
    j["spanId"]                 = encoding::hex::encode(value.span_id);
    j["traceState"]             = value.trace_state;
    j["attributes"]             = value.attributes;
    j["droppedAttributesCount"] = value.dropped_attributes_count;
    j["flags"]                  = value.flags;
}

void to_json(nlohmann::json& j, const status& value)
{
    j["message"] = value.message;
    j["code"]    = value.code;
}

void to_json(nlohmann::json& j, const span& value)
{
    j["traceId"]                = encoding::hex::encode(value.trace_id);
    j["spanId"]                 = encoding::hex::encode(value.span_id);
    j["name"]                   = value.name;
    j["kind"]                   = value.kind;
    j["startTimeUnixNano"]      = value.start_time.time_since_epoch().count();
    j["endTimeUnixNano"]        = value.end_time.time_since_epoch().count();
    j["attributes"]             = value.attributes;
    j["droppedAttributesCount"] = value.dropped_attributes_count;
    j["events"]                 = value.events;
    j["droppedEventsCount"]     = value.dropped_events_count;
    j["links"]                  = value.links;
    j["droppedLinksCount"]      = value.dropped_links_count;
    j["status"]                 = value.status;

    if (!value.trace_state.empty()) j["traceState"] = value.trace_state;
    if (value.parent_span_id.has_value()) j["parentSpanId"] = encoding::hex::encode(value.parent_span_id.value());
    if (value.flags != span_flags::zero) j["flags"] = value.flags;
}

void to_json(nlohmann::json& j, const instrumentation_scope& value)
{
    j["name"]                   = value.name;
    j["version"]                = value.version;
    j["attributes"]             = value.attributes;
    j["droppedAttributesCount"] = value.dropped_attributes_count;
}

void to_json(nlohmann::json& j, const scope_spans& value)
{
    j["scope"]     = value.scope;
    j["spans"]     = value.spans;
    j["schemaUrl"] = value.schema_url;
}

void to_json(nlohmann::json& j, const resource_spans& value)
{
    j["resource"]   = value.resource;
    j["scopeSpans"] = value.scope_spans;
    j["schemaUrl"]  = value.schema_url;
}

void to_json(nlohmann::json& j, const exemplar& value)
{
    j["filteredAttributes"] = value.filtered_attributes;
    j["timeUnixNano"]       = value.time.time_since_epoch().count();
    std::visit(
        util::overloaded{
            [&](double v) { j["asDouble"] = v; },
            [&](std::int64_t v) { j["asInt"] = v; },
        },
        value.value);
    j["spanId"]  = encoding::hex::encode(value.span_id);
    j["traceId"] = encoding::hex::encode(value.trace_id);
}

void to_json(nlohmann::json& j, const number_data_point& value)
{
    j["attributes"]        = value.attributes;
    j["startTimeUnixNano"] = value.start_time.time_since_epoch().count();
    j["timeUnixNano"]      = value.time.time_since_epoch().count();
    std::visit(
        util::overloaded{
            [&](double v) { j["asDouble"] = v; },
            [&](std::int64_t v) { j["asInt"] = v; },
        },
        value.value);
    j["exemplars"] = value.exemplars;
    j["flags"]     = std::to_underlying(value.flags);
}

void to_json(nlohmann::json& j, const gauge& value) { j["dataPoints"] = value.data_points; }

void to_json(nlohmann::json& j, const sum& value)
{
    j["dataPoints"]             = value.data_points;
    j["aggregationTemporality"] = std::to_underlying(value.aggregation_temporality);
    j["isMonotonic"]            = value.is_monotonic;
}

void to_json(nlohmann::json& j, const histogram::data_point& value)
{
    j["attributes"]        = value.attributes;
    j["startTimeUnixNano"] = value.start_time.time_since_epoch().count();
    j["timeUnixNano"]      = value.time.time_since_epoch().count();
    j["count"]             = value.count;
    if (value.sum.has_value()) j["sum"] = value.sum.value();
    j["bucketCounts"]   = value.bucket_counts;
    j["explicitBounds"] = value.explicit_bounds;
    j["exemplars"]      = value.exemplars;
    j["flags"]          = std::to_underlying(value.flags);
    if (value.min.has_value()) j["min"] = value.min.value();
    if (value.max.has_value()) j["max"] = value.max.value();
}

void to_json(nlohmann::json& j, const histogram& value)
{
    j["dataPoints"]             = value.data_points;
    j["aggregationTemporality"] = std::to_underlying(value.aggregation_temporality);
}

void to_json(nlohmann::json& j, const exponential_histogram::data_point::buckets& value)
{
    j["offset"]       = value.offset;
    j["bucketCounts"] = value.bucket_counts;
}

void to_json(nlohmann::json& j, const exponential_histogram::data_point& value)
{
    j["attributes"]        = value.attributes;
    j["startTimeUnixNano"] = value.start_time.time_since_epoch().count();
    j["timeUnixNano"]      = value.time.time_since_epoch().count();
    j["count"]             = value.count;
    if (value.sum.has_value()) j["sum"] = value.sum.value();
    j["zeroCount"] = value.zero_count;
    j["positive"]  = value.positive;
    j["negative"]  = value.negative;
    j["flags"]     = std::to_underlying(value.flags);
    j["exemplars"] = value.exemplars;
    if (value.min.has_value()) j["min"] = value.min.value();
    if (value.max.has_value()) j["max"] = value.max.value();
    j["zeroThreshold"] = value.zero_threshold;
}

void to_json(nlohmann::json& j, const exponential_histogram& value)
{
    j["dataPoints"]             = value.data_points;
    j["aggregationTemporality"] = std::to_underlying(value.aggregation_temporality);
}

void to_json(nlohmann::json& j, const summary::data_point::value_at_quantile& value)
{
    j["quantile"] = value.quantile;
    j["value"]    = value.value;
}

void to_json(nlohmann::json& j, const summary::data_point& value)
{
    j["attributes"]        = value.attributes;
    j["startTimeUnixNano"] = value.start_time.time_since_epoch().count();
    j["timeUnixNano"]      = value.time.time_since_epoch().count();
    j["count"]             = value.count;
    j["sum"]               = value.sum;
    j["quantileValues"]    = value.quantile_values;
    j["flags"]             = std::to_underlying(value.flags);
}

void to_json(nlohmann::json& j, const summary& value) { j["dataPoints"] = value.data_points; }

void to_json(nlohmann::json& j, const metric& value)
{
    j["name"]        = value.name;
    j["description"] = value.description;
    j["unit"]        = value.unit;
    std::visit(
        util::overloaded{
            [&](const gauge& v) { j["gauge"] = v; },
            [&](const sum& v) { j["sum"] = v; },
            [&](const histogram& v) { j["histogram"] = v; },
            [&](const exponential_histogram& v) { j["exponentialHistogram"] = v; },
            [&](const summary& v) { j["summary"] = v; },
        },
        value.data);
    j["metadata"] = value.metadata;
}

void to_json(nlohmann::json& j, const scope_metrics& value)
{
    j["scope"]     = value.scope;
    j["metrics"]   = value.metrics;
    j["schemaUrl"] = value.schema_url;
}

void to_json(nlohmann::json& j, const resource_metrics& value)
{
    j["resource"]     = value.resource;
    j["scopeMetrics"] = value.scope_metrics;
    j["schemaUrl"]    = value.schema_url;
}

void to_json(nlohmann::json& j, const export_metrics_service_request& value)
{
    j["resourceMetrics"] = value.resource_metrics;
}

void from_json(const nlohmann::json& j, export_metrics_partial_success& value)
{
    value.rejected_data_points = j.value("rejectedDataPoints", 0);
    value.error_message        = j.value("errorMessage", "");
}

void from_json(const nlohmann::json& j, export_metrics_service_response& value)
{
    auto it = j.find("partialSuccess");
    if (it != j.end())
    {
        export_metrics_partial_success v = *it;
        value.partial_success            = std::make_optional(v);
    }
}

void to_json(nlohmann::json& j, const export_trace_service_request& value)
{
    j["resourceSpans"] = value.resource_spans;
}

void from_json(const nlohmann::json& j, export_trace_partial_success& value)
{
    value.rejected_spans = j.value("rejectedSpans", 0);
    value.error_message  = j.value("errorMessage", "");
}

void from_json(const nlohmann::json& j, export_trace_service_response& value)
{
    if (j.contains("partialSuccess")) value.partial_success = j["partialSuccess"];
    else value.partial_success = std::nullopt;
}

}
