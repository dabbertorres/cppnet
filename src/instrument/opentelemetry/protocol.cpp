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

// TODO: consider dropping the from_json()s - not needed for our use-case.

using json = nlohmann::json;

bool operator==(const key_value_list& lhs, const key_value_list& rhs) noexcept { return lhs.values == rhs.values; }

void to_json(nlohmann::json& j, const key_value_list& value) { j["values"] = value.values; }
void from_json(const nlohmann::json& j, key_value_list& value) { j.at("values").get_to(value.values); }

bool operator==(const array_value& lhs, const array_value& rhs) noexcept { return lhs.values == rhs.values; }

void to_json(nlohmann::json& j, const array_value& value) { j["values"] = value.values; }
void from_json(const nlohmann::json& j, array_value& value) { j.at("values").get_to(value.values); }

bool operator==(const any_value& lhs, const any_value& rhs) noexcept { return lhs.value == rhs.value; }

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

void from_json(const nlohmann::json& j, any_value& value)
{
    if (auto it = j.find("stringValue"); it != j.end())
    {
        value.value = it->template get<std::string>();
    }
    else if (it = j.find("boolValue"); it != j.end())
    {
        value.value = it->template get<bool>();
    }
    else if (it = j.find("intValue"); it != j.end())
    {
        value.value = it->template get<std::int64_t>();
    }
    else if (it = j.find("doubleValue"); it != j.end())
    {
        value.value = it->template get<double>();
    }
    else if (it = j.find("arrayValue"); it != j.end())
    {
        value.value = it->template get<array_value>();
    }
    else if (it = j.find("kvlistValue"); it != j.end())
    {
        value.value = it->template get<key_value_list>();
    }
    else if (it = j.find("bytesValue"); it != j.end())
    {
        encoding::base64::encoding enc;

        auto encoded = it->template get<std::string>();
        auto raw     = enc.decode({encoded.begin(), encoded.end()});

        // let the exception propagate to indicate failure
        value.value = raw.value(); // NOLINT(bugprone-unchecked-optional-access)
    }
    else
    {
        // noop
    }
}

bool operator==(const key_value& lhs, const key_value& rhs) noexcept
{
    return lhs.key == rhs.key && lhs.value == rhs.value;
}

void to_json(nlohmann::json& j, const key_value& value)
{
    j["key"]   = value.key;
    j["value"] = value.value;
}

void from_json(const nlohmann::json& j, key_value& value)
{
    j.at("key").get_to(value.key);
    j.at("value").get_to(value.value);
}

void to_json(nlohmann::json& j, const resource& value)
{
    j["attributes"]             = value.attributes;
    j["droppedAttributesCount"] = value.dropped_attributes_count;
}

void from_json(const nlohmann::json& j, resource& value)
{
    j.at("attributes").get_to(value.attributes);
    j.at("droppedAttributesCount").get_to(value.dropped_attributes_count);
}

void to_json(nlohmann::json& j, const event& value)
{
    j["timeUnixNano"]           = value.time_unix_nano.time_since_epoch().count();
    j["name"]                   = value.name;
    j["attributes"]             = value.attributes;
    j["droppedAttributesCount"] = value.dropped_attributes_count;
}

void from_json(const nlohmann::json& j, event& value)
{
    std::uint64_t raw_nanos = 0;
    j.at("timeUnixNano").get_to(raw_nanos);
    value.time_unix_nano = time_point{time_point::duration{raw_nanos}};

    j.at("name").get_to(value.name);
    j.at("attributes").get_to(value.attributes);
    j.at("droppedAttributesCount").get_to(value.dropped_attributes_count);
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

void from_json(const nlohmann::json& j, link& value)
{
    auto raw_trace_id = j.at("traceId").template get<std::string>();
    auto trace_id     = encoding::hex::decode(raw_trace_id);

    if (trace_id.has_value()) std::copy(trace_id->begin(), trace_id->end(), value.trace_id.begin());

    auto raw_span_id = j.at("spanId").template get<std::string>();
    auto span_id     = encoding::hex::decode(raw_span_id);

    if (span_id.has_value()) std::copy(span_id->begin(), span_id->end(), value.span_id.begin());

    j.at("traceState").get_to(value.trace_state);
    j.at("attributes").get_to(value.attributes);
    j.at("droppedAttributesCount").get_to(value.dropped_attributes_count);
    j.at("flags").get_to(value.flags);
}

void to_json(nlohmann::json& j, const status& value)
{
    j["message"] = value.message;
    j["code"]    = value.code;
}

void from_json(const nlohmann::json& j, status& value)
{
    j.at("message").get_to(value.message);
    j.at("code").get_to(value.code);
}

void to_json(nlohmann::json& j, const span& value)
{
    // TODO
}

void from_json(const nlohmann::json& j, span& value)
{
    // TODO
}

void to_json(nlohmann::json& j, const instrumentation_scope& value)
{
    // TODO
}

void from_json(const nlohmann::json& j, instrumentation_scope& value)
{
    // TODO
}

void to_json(nlohmann::json& j, const scope_spans& value)
{
    // TODO
}

void from_json(const nlohmann::json& j, scope_spans& value)
{
    // TODO
}

void to_json(nlohmann::json& j, const resource_spans& value)
{
    // TODO
}

void from_json(const nlohmann::json& j, resource_spans& value)
{
    // TODO
}

void to_json(nlohmann::json& j, const export_trace_service_request& value)
{
    // TODO
}

void from_json(const nlohmann::json& j, export_trace_service_request& value)
{
    // TODO
}

void to_json(nlohmann::json& j, const export_trace_partial_success& value)
{
    // TODO
}

void from_json(const nlohmann::json& j, export_trace_partial_success& value)
{
    // TODO
}

void to_json(nlohmann::json& j, const export_trace_service_response& value)
{
    // TODO
}

void from_json(const nlohmann::json& j, export_trace_service_response& value)
{
    // TODO
}

}
