#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "http/structured_fields.hpp"

// What Structured Fields' tests and fuzz_structured_fields read parsed values into, and RFC 9651's serialization of
// them (section 4.1), which the library doesn't have (it only parses): so a parsed value can be compared with the
// canonical form the httpwg tests give, and parsed again.
namespace test
{

namespace sf = net::http::sf;

// sf_bare is a bare item, its text decoded (sf::decode()).
struct sf_bare
{
    sf::item_type type   = sf::item_type::boolean;
    std::int64_t  number = 1;
    std::string   value;

    bool operator==(const sf_bare&) const = default;
};

// sf_params are parameters as RFC 9651 has them: an ordered map, a later key replacing an earlier one in its place.
using sf_params = std::vector<std::pair<std::string, sf_bare>>;

struct sf_item
{
    sf_bare   value;
    sf_params params;

    bool operator==(const sf_item&) const = default;
};

struct sf_inner_list
{
    std::vector<sf_item> items;
    sf_params            params;

    bool operator==(const sf_inner_list&) const = default;
};

using sf_member     = std::variant<sf_item, sf_inner_list>;
using sf_list       = std::vector<sf_member>;
using sf_dictionary = std::vector<std::pair<std::string, sf_member>>; // as sf_params, in order

// put sets key in an ordered map, replacing its value where it is, or adding it at the end.
template<typename V>
void put(std::vector<std::pair<std::string, V>>& map, std::string_view key, V value)
{
    for (auto& [k, v] : map)
    {
        if (k == key)
        {
            v = std::move(value);
            return;
        }
    }
    map.emplace_back(std::string{key}, std::move(value));
}

inline sf_bare own(const sf::bare_item& b)
{
    sf_bare out{.type = b.type, .number = b.number, .value = {}};
    sf::decode(b, out.value);
    return out;
}

inline sf_params own(const sf::parameters& params)
{
    sf_params out;
    for (const auto& p : params) put(out, p.key, own(p.value));
    return out;
}

inline sf_item own(const sf::item& i) { return {.value = own(i.value), .params = own(i.params)}; }

inline sf_member own(const sf::member& m)
{
    if (const auto* i = std::get_if<sf::item>(&m)) return own(*i);
    const auto&   list = std::get<sf::inner_list>(m);
    sf_inner_list out{.items = {}, .params = own(list.params())};
    for (const auto& i : list) out.items.push_back(own(i));
    return out;
}

inline std::optional<sf_item> parse_item(std::string_view value)
{
    const auto parsed = sf::parse_item(value);
    if (!parsed) return std::nullopt;
    return own(*parsed);
}

inline std::optional<sf_list> parse_list(std::string_view value)
{
    sf_list out;
    if (!sf::parse_list(value, [&](const sf::member& m) { out.push_back(own(m)); })) return std::nullopt;
    return out;
}

inline std::optional<sf_dictionary> parse_dictionary(std::string_view value)
{
    sf_dictionary out;
    if (!sf::parse_dictionary(value, [&](std::string_view key, const sf::member& m) { put(out, key, own(m)); }))
    {
        return std::nullopt;
    }
    return out;
}

// serialize_decimal is section 4.1.5's, of thousandths.
inline std::string serialize_decimal(std::int64_t thousandths)
{
    std::string out;
    if (thousandths < 0) out += '-';
    const auto magnitude = static_cast<std::uint64_t>(std::llabs(thousandths));
    out += std::to_string(magnitude / 1'000);
    out += '.';
    auto fraction = std::to_string(magnitude % 1'000);
    fraction.insert(0, 3 - fraction.size(), '0');
    while (fraction.size() > 1 && fraction.back() == '0') fraction.pop_back();
    return out + fraction;
}

// serialize_bytes is section 4.1.8's: base64, padded.
inline std::string serialize_bytes(std::string_view bytes)
{
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string                out      = ":";
    for (std::size_t i = 0; i < bytes.size(); i += 3)
    {
        const std::size_t n    = std::min<std::size_t>(3, bytes.size() - i);
        std::uint32_t     bits = 0;
        for (std::size_t j = 0; j < 3; ++j)
        {
            bits = (bits << 8) | (j < n ? static_cast<unsigned char>(bytes[i + j]) : 0U);
        }
        for (std::size_t j = 0; j < 4; ++j) out += j <= n ? alphabet[(bits >> (18 - (6 * j))) & 0x3f] : '=';
    }
    return out + ':';
}

// serialize_display is section 4.1.11's: percent-encoding what isn't printable ASCII, and '%' and '"'.
inline std::string serialize_display(std::string_view utf8)
{
    constexpr std::string_view hex = "0123456789abcdef";
    std::string                out = "%\"";
    for (const char c : utf8)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '%' || c == '"' || byte < 0x20 || byte > 0x7e)
        {
            out += '%';
            out += hex[byte >> 4];
            out += hex[byte & 0xf];
        }
        else
        {
            out += c;
        }
    }
    return out + '"';
}

// serialize is section 4.1's, for each.
inline std::string serialize(const sf_bare& b)
{
    switch (b.type)
    {
    case sf::item_type::integer: return std::to_string(b.number);
    case sf::item_type::decimal: return serialize_decimal(b.number);
    case sf::item_type::string:
    {
        std::string out = "\"";
        for (const char c : b.value)
        {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out + '"';
    }
    case sf::item_type::token: return b.value;
    case sf::item_type::byte_sequence: return serialize_bytes(b.value);
    case sf::item_type::boolean: return b.number != 0 ? "?1" : "?0";
    case sf::item_type::date: return "@" + std::to_string(b.number);
    case sf::item_type::display_string: return serialize_display(b.value);
    }
    return {};
}

inline bool is_true(const sf_bare& b) { return b.type == sf::item_type::boolean && b.number != 0; }

inline std::string serialize(const sf_params& params)
{
    std::string out;
    for (const auto& [key, value] : params)
    {
        out += ';';
        out += key;
        if (!is_true(value)) out += "=" + serialize(value);
    }
    return out;
}

inline std::string serialize(const sf_item& i) { return serialize(i.value) + serialize(i.params); }

inline std::string serialize(const sf_member& m)
{
    if (const auto* i = std::get_if<sf_item>(&m)) return serialize(*i);
    const auto& list = std::get<sf_inner_list>(m);
    std::string out  = "(";
    for (const auto& i : list.items)
    {
        if (out.size() > 1) out += ' ';
        out += serialize(i);
    }
    return out + ")" + serialize(list.params);
}

inline std::string serialize(const sf_list& list)
{
    std::string out;
    for (const auto& m : list)
    {
        if (!out.empty()) out += ", ";
        out += serialize(m);
    }
    return out;
}

inline std::string serialize(const sf_dictionary& dict)
{
    std::string out;
    for (const auto& [key, m] : dict)
    {
        if (!out.empty()) out += ", ";
        out += key;
        // A member that's Boolean true is its key alone (and its parameters).
        if (const auto* i = std::get_if<sf_item>(&m); i != nullptr && is_true(i->value)) out += serialize(i->params);
        else out += "=" + serialize(m);
    }
    return out;
}

}
