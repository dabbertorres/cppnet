#include "instrument/opentelemetry/protocol.hpp"

#include <algorithm>
#include <cstdint>
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
    // TODO
}

void to_json(nlohmann::json& j, const instrumentation_scope& value)
{
    // TODO
}

void to_json(nlohmann::json& j, const scope_spans& value)
{
    // TODO
}

void to_json(nlohmann::json& j, const resource_spans& value)
{
    // TODO
}

void to_json(nlohmann::json& j, const export_trace_service_request& value)
{
    // TODO
}

void to_json(nlohmann::json& j, const export_trace_partial_success& value)
{
    // TODO
}

void to_json(nlohmann::json& j, const export_trace_service_response& value)
{
    // TODO
}

}
