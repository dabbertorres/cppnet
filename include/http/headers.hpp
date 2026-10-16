#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "util/small_vector.hpp"

namespace net::http
{

struct content_type
{
    std::string                                  type{};
    std::unordered_map<std::string, std::string> parameters{};
    [[nodiscard]] std::optional<std::string>     charset() const noexcept
    {
        auto it = parameters.find("charset");
        if (it != parameters.end()) return it->second;
        return std::nullopt;
    }
};

// headers are a message's fields: each name's values, one per field line, in the order they were received or set.
// Names are compared ignoring case.
//
// NOTE: a message has a few fields, so they're kept in order (each name once), and looked up by going through them:
//       that, and holding the first few (and the first value of each) in itself, saves allocating for each one. Fields
//       removed (or cleared) are kept for their memory, which fields added later reuse: so headers reused for each
//       message on a connection (see clear()) needn't allocate for values that are too long to hold in themselves.
class headers
{
public:
    using values_type = util::small_vector<std::string, 1>;
    using field_type  = std::pair<std::string, values_type>;

    using value_iterator = values_type::const_iterator;
    using keys_iterator  = const field_type*;

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
    headers(std::initializer_list<std::pair<std::string, std::vector<std::string>>> init);

    headers(const headers&)            = default;
    headers& operator=(const headers&) = default;

    headers(headers&& other) noexcept;
    headers& operator=(headers&& other) noexcept;

    ~headers() = default;

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

    // remove removes key's values, if it has any.
    headers& remove(std::string_view key);

    // clear removes every field, keeping their memory for the fields added next.
    void clear() noexcept;

    headers& set_content_length(std::size_t length);
    headers& set_content_type(const content_type& content_type);

    // NOTE: the views get() and get_all() return are only valid until the headers are next changed (as a vector's
    //       elements' are), as the fields are held in order, rather than one by one.
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

    // has_token is whether a list-based field has the element token (ignoring case), e.g. "close" in "Connection:
    // close", without allocating for the list.
    [[nodiscard]] bool has_token(std::string_view key, std::string_view token) const;

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

    // headers are equal if they have the same values for each name, in the same order, whatever order the names are in.
    friend bool operator==(const headers& lhs, const headers& rhs) noexcept;

private:
    [[nodiscard]] const field_type* find(std::string_view key) const noexcept;
    [[nodiscard]] field_type*       find(std::string_view key) noexcept;

    // for_each_element calls visit with each element of a list-based field (see get_list), until it returns false.
    template<typename F>
    void for_each_element(std::string_view key, F visit) const;

    // add_field adds key, with no values, reusing a removed field's memory if there is one. Its values must then be
    // set (with set_one, or after clearing them), as they may still be the removed field's.
    field_type& add_field(std::string_view key);

    // assign returns key's values, emptied, adding key if it has none.
    values_type& assign(std::string_view key);

    // set_one sets key's value to val, reusing the memory of its first value (or a removed field's).
    template<typename V>
    headers& set_one(std::string_view key, V&& val);

    // fields holds the fields (the first used), and then those removed, kept for their memory.
    util::small_vector<field_type, 8> fields;
    std::size_t                       used = 0;
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
