#pragma once

#include <cctype>
#include <concepts>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace net::http
{

class headers
{
private:
    struct keyhash
    {
        size_t operator()(const std::string& key) const noexcept;
    };

    struct keyequal
    {
        bool operator()(const std::string& lhs, const std::string& rhs) const noexcept;
    };

    using value_type = std::vector<std::string>;
    using key_type   = std::string;
    using map_type   = std::unordered_map<key_type, value_type, keyhash, keyequal>;

public:
    using value_iterator = value_type::const_iterator;
    using values_range   = std::pair<value_iterator, value_iterator>;
    using keys_iterator  = map_type::const_iterator;

    headers() = default;
    headers(std::initializer_list<map_type::value_type> init);

    headers& set(const std::string& key, std::string val);
    headers& set(const std::string& key, std::initializer_list<std::string> vals);
    headers& add(const std::string& key, std::string val);

    headers& set(std::string_view key, std::string val);
    headers& set(std::string_view key, std::initializer_list<std::string> vals);
    headers& add(std::string_view key, std::string val);

    headers& set(const std::string& key, std::string_view val);
    headers& set(const std::string& key, std::initializer_list<std::string_view> vals);
    headers& add(const std::string& key, std::string_view val);

    headers& set(std::string_view key, std::string_view val);
    headers& set(std::string_view key, std::initializer_list<std::string_view> vals);
    headers& add(std::string_view key, std::string_view val);

    [[nodiscard]] std::optional<std::string_view> get(const std::string& key) const;
    [[nodiscard]] std::optional<std::string_view> operator[](const std::string& key) const;

    [[nodiscard]] std::optional<values_range> get_all(const std::string& key) const;

    [[nodiscard]] keys_iterator begin() const;
    [[nodiscard]] keys_iterator end() const;

    [[nodiscard]] bool   empty() const noexcept;
    [[nodiscard]] size_t size() const noexcept;

private:
    map_type values;
};

bool operator==(const headers& lhs, const headers& rhs) noexcept;

}
