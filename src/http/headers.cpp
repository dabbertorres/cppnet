#include "http/headers.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "util/string_util.hpp"

namespace
{

// trim_ows trims optional whitespace (spaces and tabs, RFC 9110 section 5.6.3) from both ends of str.
// NOLINTNEXTLINE(bugprone-exception-escape): substr's positions are in range.
std::string_view trim_ows(std::string_view str) noexcept
{
    constexpr auto ows = " \t";

    auto start = str.find_first_not_of(ows);
    if (start == std::string_view::npos) return {};

    return str.substr(start, str.find_last_not_of(ows) - start + 1);
}

// find_field finds key's field in the first used of fields (const or not), or returns null.
auto* find_field(auto& fields, std::size_t used, std::string_view key) noexcept
{
    auto* first = fields.data();
    auto* last  = first + used;
    auto* it    = std::find_if(first,
                               last,
                               [&](const auto& field)
                               { return net::util::equal_ignore_case(std::string_view{field.first}, key); });
    return it != last ? it : nullptr;
}

// set_values makes vals just val, reusing the memory of the first value.
template<typename V>
void set_values(auto& vals, V&& val)
{
    if (vals.empty())
    {
        vals.emplace_back(std::forward<V>(val));
        return;
    }

    if constexpr (std::is_same_v<std::remove_cvref_t<V>, std::string> && !std::is_lvalue_reference_v<V>)
        vals.front() = std::forward<V>(val);
    else vals.front().assign(val);

    while (vals.size() > 1) vals.erase(vals.end() - 1);
}
}

namespace net::http
{

using namespace std::string_literals;
using namespace std::string_view_literals;

headers::headers(std::initializer_list<std::pair<std::string, std::vector<std::string>>> init)
{
    for (const auto& [key, vals] : init)
    {
        for (const auto& val : vals) add(key, val);
    }
}

headers::headers(headers&& other) noexcept
    : fields{std::move(other.fields)}
    , used{std::exchange(other.used, 0)}
{}

headers& headers::operator=(headers&& other) noexcept
{
    fields = std::move(other.fields);
    used   = std::exchange(other.used, 0);
    return *this;
}

const headers::field_type* headers::find(std::string_view key) const noexcept { return find_field(fields, used, key); }

headers::field_type* headers::find(std::string_view key) noexcept { return find_field(fields, used, key); }

headers::field_type& headers::add_field(std::string_view key)
{
    if (used < fields.size())
    {
        auto& field = fields[used++];
        field.first.assign(key);
        return field;
    }

    auto& field = fields.emplace_back(std::string{key}, values_type{});
    ++used;
    return field;
}

headers::values_type& headers::assign(std::string_view key)
{
    auto* field = find(key);
    auto& vals  = field != nullptr ? field->second : add_field(key).second;
    vals.clear();
    return vals;
}

template<typename V>
headers& headers::set_one(std::string_view key, V&& val)
{
    auto* field = find(key);
    set_values(field != nullptr ? field->second : add_field(key).second, std::forward<V>(val));
    return *this;
}

headers& headers::set(std::string_view key, std::string val) { return set_one(key, std::move(val)); }

headers& headers::set(std::string_view key, std::vector<std::string>&& vals)
{
    auto& out   = assign(key);
    auto  taken = std::move(vals);
    for (auto& val : taken) out.push_back(std::move(val));
    return *this;
}

headers& headers::add(std::string_view key, std::string val)
{
    if (auto* field = find(key); field != nullptr) field->second.push_back(std::move(val));
    else set_values(add_field(key).second, std::move(val));
    return *this;
}

headers& headers::set(std::string_view key, std::string_view val) { return set_one(key, val); }

headers& headers::set(std::string_view key, std::initializer_list<std::string_view> vals)
{
    auto& out = assign(key);
    for (auto val : vals) out.emplace_back(val);
    return *this;
}

headers& headers::add(std::string_view key, std::string_view val)
{
    if (auto* field = find(key); field != nullptr) field->second.emplace_back(val);
    else set_values(add_field(key).second, val);
    return *this;
}

headers& headers::remove(std::string_view key)
{
    auto* field = find(key);
    if (field == nullptr) return *this;

    // moved after the fields, keeping its memory.
    std::rotate(field, field + 1, fields.data() + used);
    --used;
    return *this;
}

void headers::clear() noexcept { used = 0; }

headers& headers::set_content_length(std::size_t length) { return set("Content-Length"sv, std::to_string(length)); }

headers& headers::set_content_type(const content_type& content_type)
{
    // nothing to add
    if (content_type.type.empty()) return *this;

    std::ostringstream ss;
    ss << content_type.type;

    std::ranges::for_each(content_type.parameters,
                          [&](const auto& param) { ss << ": " << param.first << '=' << param.second; });

    return set("Content-Type"sv, ss.str());
}

[[nodiscard]] std::optional<std::string_view> headers::get(std::string_view key) const
{
    const auto* field = find(key);
    if (field == nullptr || field->second.empty()) return std::nullopt;

    return std::string_view{field->second.front()};
}

[[nodiscard]] std::optional<std::string_view> headers::operator[](std::string_view key) const { return get(key); }

[[nodiscard]] std::optional<headers::values_range> headers::get_all(std::string_view key) const
{
    const auto* field = find(key);
    if (field == nullptr) return std::nullopt;

    return values_range{field->second.begin(), field->second.end()};
}

std::vector<std::string_view> headers::get_list(std::string_view key) const
{
    std::vector<std::string_view> out;
    for_each_element(key,
                     [&](std::string_view element)
                     {
                         out.push_back(element);
                         return true;
                     });
    return out;
}

bool headers::has_token(std::string_view key, std::string_view token) const
{
    bool found = false;
    for_each_element(key,
                     [&](std::string_view element)
                     {
                         found = util::equal_ignore_case(element, token);
                         return !found;
                     });
    return found;
}

template<typename F>
void headers::for_each_element(std::string_view key, F visit) const
{
    const auto* field = find(key);
    if (field == nullptr) return;

    for (std::string_view value : field->second)
    {
        bool        quoted  = false;
        std::size_t start   = 0;
        const auto  element = [&](std::size_t end)
        {
            auto trimmed = trim_ows(value.substr(start, end - start));
            start        = end + 1;
            return trimmed.empty() || visit(trimmed);
        };

        for (std::size_t i = 0; i < value.size(); ++i)
        {
            if (quoted && value[i] == '\\') ++i; // skip what's escaped
            else if (value[i] == '"') quoted = !quoted;
            else if (!quoted && value[i] == ',' && !element(i)) return;
        }

        if (!element(value.size())) return;
    }
}

[[nodiscard]] std::optional<std::size_t> headers::get_content_length() const
{
    std::optional<std::size_t> length;

    for (auto value : get_list("Content-Length"sv))
    {
        // only digits: from_chars would also take a leading '-', and stop at the first non-digit.
        if (!std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;

        std::size_t parsed = 0;
        auto [ptr, err]    = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (err != std::errc() || ptr != value.data() + value.size()) return std::nullopt;

        // repeated values must agree.
        if (length.has_value() && *length != parsed) return std::nullopt;
        length = parsed;
    }

    return length;
}

[[nodiscard]] std::optional<content_type> headers::get_content_type() const
{
    auto maybe = get("Content-Type"sv);
    if (!maybe.has_value()) return std::nullopt;

    const std::string_view type  = maybe.value();
    const std::size_t      split = type.find(';');
    if (split == std::string_view::npos) return content_type{.type = std::string{type}};

    content_type out{.type = std::string{type}};

    for (auto param : util::split_string(maybe.value(), ';', split + 1))
    {
        auto kv_split = param.find('=');
        if (kv_split == std::string_view::npos)
        {
            out.parameters.emplace(param, "");
            continue;
        }

        auto key = util::trim_string(param.substr(0, kv_split));
        auto val = util::trim_string(param.substr(kv_split + 1));
        val      = util::unquote(val);

        // charset is a known parameter defined as case-insensitive, standardized as lowercase
        if (util::equal_ignore_case(key, "charset"sv))
        {
            constexpr auto to_lower = [](char& c) { c = static_cast<char>(std::tolower(c)); };

            std::string key_v{key};
            std::string val_v{val};

            std::ranges::for_each(key_v, to_lower);
            std::ranges::for_each(val_v, to_lower);

            out.parameters.emplace(key_v, val_v);
        }
        else
        {
            out.parameters.emplace(key, val);
        }
    }

    return out;
}

[[nodiscard]] std::optional<headers::values_range> headers::get_content_encoding() const
{
    return get_all("Content-Encoding"sv);
}

[[nodiscard]] std::optional<headers::values_range> headers::get_transfer_encoding() const
{
    return get_all("Transfer-Encoding"sv);
}

[[nodiscard]] bool headers::is_chunked() const
{
    auto codings = get_list("Transfer-Encoding"sv);
    return !codings.empty() && util::equal_ignore_case(codings.back(), "chunked"sv);
}

[[nodiscard]] headers::keys_iterator headers::begin() const { return fields.data(); }
[[nodiscard]] headers::keys_iterator headers::end() const { return fields.data() + used; }

[[nodiscard]] bool headers::empty() const noexcept { return used == 0; }

[[nodiscard]] std::size_t headers::size() const noexcept { return used; }

bool operator==(const headers& lhs, const headers& rhs) noexcept
{
    return lhs.size() == rhs.size()
        && std::ranges::all_of(lhs,
                               [&](const auto& field)
                               {
                                   const auto* other = rhs.find(field.first);
                                   return other != nullptr && other->second == field.second;
                               });
}

bool is_valid_field_name(std::string_view name) noexcept
{
    constexpr auto is_tchar = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || "!#$%&'*+-.^_`|~"sv.contains(c);
    };

    return !name.empty() && std::ranges::all_of(name, is_tchar);
}

bool is_valid_field_value(std::string_view value) noexcept
{
    // anything but controls, other than tab: visible ASCII, space, and (obsolete, but allowed) bytes above ASCII.
    return std::ranges::none_of(value,
                                [](char c)
                                {
                                    auto byte = static_cast<unsigned char>(c);
                                    return (byte < 0x20 && byte != '\t') || byte == 0x7F;
                                });
}

// NOLINTNEXTLINE(bugprone-exception-escape): substr's positions are in range.
std::optional<std::pair<std::string_view, std::string_view>> parse_field_line(std::string_view line) noexcept
{
    auto colon = line.find(':');
    if (colon == std::string_view::npos) return std::nullopt;

    // no whitespace is allowed before the colon (RFC 9112, section 5.1), which also rules out line folding.
    auto name = line.substr(0, colon);
    if (!is_valid_field_name(name)) return std::nullopt;

    auto value = trim_ows(line.substr(colon + 1));
    if (!is_valid_field_value(value)) return std::nullopt;

    return std::pair{name, value};
}

}
