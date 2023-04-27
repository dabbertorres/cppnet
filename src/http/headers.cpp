#include "http/headers.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "util/string_util.hpp"

namespace net::http
{

using namespace std::string_view_literals;

std::size_t headers::keyhash::operator()(const std::string& key) const noexcept
{
    return operator()(static_cast<std::string_view>(key));
}

std::size_t headers::keyhash::operator()(std::string_view key) const noexcept
{
    // variant of Fowler-Noll-Vo

    std::size_t result = 2'166'136'261;

    for (auto c : key)
    {
        result = (result * 16'777'619) ^ static_cast<std::size_t>(std::tolower(c));
    }

    return result;
}

bool headers::keyequal::operator()(const std::string& lhs, const std::string& rhs) const noexcept
{
    return net::util::equal_ignore_case(lhs, rhs);
}

bool headers::keyequal::operator()(std::string_view lhs, const std::string& rhs) const noexcept
{
    return net::util::equal_ignore_case(lhs, static_cast<std::string_view>(rhs));
}

bool headers::keyequal::operator()(const std::string& lhs, std::string_view rhs) const noexcept
{
    return net::util::equal_ignore_case(static_cast<std::string_view>(lhs), rhs);
}

headers::headers(std::initializer_list<map_type::value_type> init)
    : values(init)
{}

headers& headers::set(const std::string& key, std::string val)
{
    values.insert_or_assign(key, std::vector{std::move(val)});
    return *this;
}

headers& headers::set(const std::string& key, std::initializer_list<std::string> vals)
{
    values.insert_or_assign(key, vals);
    return *this;
}

headers& headers::add(const std::string& key, std::string val)
{
    values[key].push_back(std::move(val));
    return *this;
}

headers& headers::set(std::string_view key, std::string val) { return set(std::string(key), std::move(val)); }

headers& headers::set(std::string_view key, std::initializer_list<std::string> vals)
{
    return set(std::string(key), vals);
}

headers& headers::add(std::string_view key, std::string val) { return add(std::string(key), std::move(val)); }

headers& headers::set(const std::string& key, std::string_view val) { return set(key, std::string(val)); }

headers& headers::set(const std::string& key, std::initializer_list<std::string_view> vals)
{
    value_type tmp;
    tmp.resize(vals.size());

    std::transform(vals.begin(), vals.end(), tmp.begin(), [](std::string_view view) { return std::string(view); });

    values[key] = tmp;
    return *this;
}

headers& headers::add(const std::string& key, std::string_view val)
{
    values[key].emplace_back(val);
    return *this;
}

headers& headers::set(std::string_view key, std::string_view val) { return set(std::string(key), std::string(val)); }

headers& headers::set(std::string_view key, std::initializer_list<std::string_view> vals)
{
    return set(std::string(key), vals);
}

headers& headers::add(std::string_view key, std::string_view val) { return add(std::string(key), val); }

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

[[nodiscard]] std::optional<std::string_view> headers::get(const std::string& key) const
{
    auto iter = values.find(key);
    if (iter == values.end()) return std::nullopt;
    if (iter->second.empty()) return std::nullopt;

    std::string_view view = iter->second.front();
    return {view};
}

[[nodiscard]] std::optional<std::string_view> headers::get(std::string_view key) const
{
    auto iter = values.find(key);
    if (iter == values.end()) return std::nullopt;
    if (iter->second.empty()) return std::nullopt;

    std::string_view view = iter->second.front();
    return {view};
}

[[nodiscard]] std::optional<std::string_view> headers::operator[](const std::string& key) const { return get(key); }
[[nodiscard]] std::optional<std::string_view> headers::operator[](std::string_view key) const { return get(key); }

[[nodiscard]] std::optional<headers::values_range> headers::get_all(const std::string& key) const
{
    return get_all(std::string_view{key});
}

[[nodiscard]] std::optional<headers::values_range> headers::get_all(std::string_view key) const
{
    auto iter = values.find(key);
    if (iter == values.end()) return std::nullopt;

    return {
        {iter->second.begin(), iter->second.end()}
    };
}

[[nodiscard]] std::optional<std::size_t> headers::get_content_length() const
{
    auto maybe = get("Content-Length"sv);
    if (!maybe.has_value()) return std::nullopt;

    std::size_t length; // NOLINT(cppcoreguidelines-init-variables)
    auto [_, err] = std::from_chars(maybe->begin(), maybe->end(), length);

    if (err != std::errc()) return std::nullopt;
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

            std::for_each(key_v.begin(), key_v.end(), to_lower);
            std::for_each(val_v.begin(), val_v.end(), to_lower);

            out.parameters.emplace(key_v, val_v);
        }
        else
        {
            out.parameters.emplace(key, val);
        }
    }

    return out;
}

[[nodiscard]] bool headers::is_compressed() const
{
    auto maybe = get("Content-Encoding"sv);
    if (!maybe.has_value()) return false;

    auto val = maybe.value();
    return util::equal_ignore_case(val, "compress"sv) || util::equal_ignore_case(val, "x-compress"sv);
}

[[nodiscard]] bool headers::is_deflated() const
{
    auto maybe = get("Content-Encoding"sv);
    if (!maybe.has_value()) return false;

    auto val = maybe.value();
    return util::equal_ignore_case(val, "deflate"sv) || util::equal_ignore_case(val, "zlib"sv);
}

[[nodiscard]] bool headers::is_gziped() const
{
    auto maybe = get("Content-Encoding"sv);
    if (!maybe.has_value()) return false;

    auto val = maybe.value();
    return util::equal_ignore_case(val, "gzip"sv) || util::equal_ignore_case(val, "x-gzip"sv);
}

[[nodiscard]] headers::keys_iterator headers::begin() const { return values.begin(); }
[[nodiscard]] headers::keys_iterator headers::end() const { return values.end(); }

[[nodiscard]] bool headers::empty() const noexcept { return values.empty(); }

[[nodiscard]] std::size_t headers::size() const noexcept { return values.size(); }

}
