#include "instrument/opentelemetry/protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <variant>

#include <nlohmann/json.hpp>

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
    j = std::visit(
        util::overloaded{
            [](const std::string& s)
            {
                return json{
                    {"stringValue", s},
                };
            },
            [](bool b)
            {
                return json{
                    {"boolValue", b},
                };
            },
            [](std::int64_t i)
            {
                return json{
                    {"intValue", i},
                };
            },
            [](double d)
            {
                return json{
                    {"doubleValue", d},
                };
            },
            [](const array_value& a)
            {
                return json{
                    {"arrayValue", {{"values", a.values}}},
                };
            },
            [](const key_value_list& l)
            {
                return json{
                    {"kvlistValue", {{"values", l.values}}},
                };
            },
            [](const std::vector<std::byte>& b)
            {
                encoding::base64::encoding enc;

                auto encoded = enc.encode({b.begin(), b.end()});
                return json{
                    {"bytesValue", encoded},
                };
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
    j["timeUnixNano"]           = value.time_unix_nano.time_since_epoch().count();
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
    j["startTimeUnixNano"]      = value.start_time_unix_nano.time_since_epoch().count();
    j["endTimeUnixNano"]        = value.end_time_unix_nano.time_since_epoch().count();
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

void to_json(nlohmann::json& j, const export_trace_service_request& value)
{
    j["resource_spans"] = value.resource_spans;
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
