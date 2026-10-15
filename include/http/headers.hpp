#pragma once

#include <cctype>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "util/string_map.hpp"

namespace net::http
{

struct content_type
{
    std::string                                  type;
    std::unordered_map<std::string, std::string> parameters;
    [[nodiscard]] std::optional<std::string>     charset() const noexcept
    {
        auto it = parameters.find("charset");
        if (it != parameters.end()) return it->second;
        return std::nullopt;
    }
};

class headers
{
private:
    struct keyhash
    {
        using is_transparent   = void;
        using string_type      = std::string;
        using string_view_type = std::string_view;

        std::size_t operator()(const std::string& key) const noexcept;
        std::size_t operator()(std::string_view key) const noexcept;
    };

    struct keyequal
    {
        using is_transparent   = void;
        using string_type      = std::string;
        using string_view_type = std::string_view;

        bool operator()(const std::string& lhs, const std::string& rhs) const noexcept;
        bool operator()(std::string_view lhs, const std::string& rhs) const noexcept;
        bool operator()(const std::string& lhs, std::string_view rhs) const noexcept;
    };

    using value_type = std::vector<std::string>;
    using key_type   = std::string;
    using map_type   = util::string_map<value_type, std::string, std::string_view, keyhash, keyequal>;

public:
    using value_iterator = value_type::const_iterator;
    using keys_iterator  = map_type::const_iterator;

    struct values_range
    {
        constexpr values_range(value_iterator begin, value_iterator end) noexcept
            : begin_it(begin)
            , end_it(end)
        {}

        [[nodiscard]] value_iterator begin() const noexcept { return begin_it; }
        [[nodiscard]] value_iterator end() const noexcept { return end_it; }

    private:
        value_iterator begin_it;
        value_iterator end_it;
    };

    headers() = default;
    headers(std::initializer_list<map_type::value_type> init);

    headers& set(const std::string& key, std::string val);
    headers& set(const std::string& key, std::initializer_list<std::string> vals);
    headers& set(const std::string& key, std::vector<std::string>&& vals);
    headers& add(const std::string& key, std::string val);

    headers& set(std::string_view key, std::string val);
    headers& set(std::string_view key, std::initializer_list<std::string> vals);
    headers& set(std::string_view key, std::vector<std::string>&& vals);
    headers& add(std::string_view key, std::string val);

    headers& set(const std::string& key, std::string_view val);
    headers& set(const std::string& key, std::initializer_list<std::string_view> vals);
    headers& add(const std::string& key, std::string_view val);

    headers& set(std::string_view key, std::string_view val);
    headers& set(std::string_view key, std::initializer_list<std::string_view> vals);
    headers& add(std::string_view key, std::string_view val);

    headers& set_content_length(std::size_t length);
    headers& set_content_type(const content_type& content_type);

    [[nodiscard]] std::optional<std::string_view> get(const std::string& key) const;
    [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const;
    [[nodiscard]] std::optional<std::string_view> operator[](const std::string& key) const;
    [[nodiscard]] std::optional<std::string_view> operator[](std::string_view key) const;

    // get_all returns every value of key: one per field line, as they were received or set.
    [[nodiscard]] std::optional<values_range> get_all(const std::string& key) const;
    [[nodiscard]] std::optional<values_range> get_all(std::string_view key) const;

    // get_list returns the elements of a list-based field (e.g. "Transfer-Encoding: gzip, chunked"), over all of its
    // field lines: each value is split on the commas that aren't in a quoted string, and empty elements are skipped.
    [[nodiscard]] std::vector<std::string_view> get_list(std::string_view key) const;

    // getters for well-known common headers

    // get_content_length returns the Content-Length, if it's present and valid: a decimal number, which every value of
    // it (if repeated) agrees on.
    [[nodiscard]] std::optional<std::size_t>  get_content_length() const;
    [[nodiscard]] std::optional<content_type> get_content_type() const;

    [[nodiscard]] std::optional<values_range> get_content_encoding() const;
    [[nodiscard]] std::optional<values_range> get_transfer_encoding() const;

    // is_chunked returns whether chunked is the last of the transfer codings, as it must be if it's used at all.
    [[nodiscard]] bool is_chunked() const;

    [[nodiscard]] keys_iterator begin() const;
    [[nodiscard]] keys_iterator end() const;

    [[nodiscard]] bool        empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    friend bool operator==(const headers& lhs, const headers& rhs) noexcept = default;

private:
    map_type values;
};

// is_valid_field_name checks that name is a token (RFC 9110, section 5.1).
[[nodiscard]] bool is_valid_field_name(std::string_view name) noexcept;

// is_valid_field_value checks that value has no control characters (CR, LF, NUL, ...) other than horizontal tab (RFC
// 9110, section 5.5).
[[nodiscard]] bool is_valid_field_value(std::string_view value) noexcept;

// parse_field_line splits a field line ("name: value") into its name and value, without the whitespace around the
// value. It fails if the line is malformed, including obsolete line folding (a line starting with whitespace).
[[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>>
parse_field_line(std::string_view line) noexcept;

}
