#include "http/headers.hpp"

#include <algorithm>
#include <utility>

#include "util/string_util.hpp"

namespace net::http
{

size_t headers::keyhash::operator()(const std::string& key) const noexcept
{
    // variant of Fowler-Noll-Vo

    size_t result = 2166136261;

    for (auto c : key)
    {
        result = (result * 16777619) ^ static_cast<size_t>(std::tolower(c));
    }

    return result;
}

bool headers::keyequal::operator()(const std::string& lhs, const std::string& rhs) const noexcept
{
    return net::util::equal_ignore_case(lhs, rhs);
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

    std::transform(vals.begin(),
                   vals.end(),
                   tmp.begin(),
                   [](std::string_view view) -> std::string { return std::string(view); });

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

[[nodiscard]] std::optional<std::string_view> headers::get(const std::string& key) const
{
    auto iter = values.find(key);
    if (iter == values.end()) return {};
    if (iter->second.empty()) return {};

    std::string_view view = iter->second.front();
    return {view};
}

[[nodiscard]] std::optional<std::string_view> headers::operator[](const std::string& key) const { return get(key); }

[[nodiscard]] std::optional<headers::values_range> headers::get_all(const std::string& key) const
{
    auto iter = values.find(key);
    if (iter == values.end()) return {};

    return {
        {iter->second.begin(), iter->second.end()}
    };
}

[[nodiscard]] headers::keys_iterator headers::begin() const { return values.begin(); }
[[nodiscard]] headers::keys_iterator headers::end() const { return values.end(); }

[[nodiscard]] bool headers::empty() const noexcept { return values.empty(); }

[[nodiscard]] size_t headers::size() const noexcept { return values.size(); }

bool operator==(const headers& lhs, const headers& rhs) noexcept
{
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

}
