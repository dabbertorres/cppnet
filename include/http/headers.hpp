#pragma once

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "util/small_vector.hpp" // IWYU pragma: export (a field's values are one)

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

// header_values are a field's values, one per field line; a header_field is a field: its name, and its values.
using header_values = util::small_vector<std::string, 1>;
using header_field  = std::pair<std::string, header_values>;

// fields_view is a view of fields (e.g. a basic_headers', whatever its N, which converts to one), for what only reads
// them (e.g. valid_fields()).
using fields_view = std::span<const header_field>;

// basic_headers are a message's fields: each name's values, one per field line, in the order they were received or
// set. Names are compared ignoring case. It holds up to N fields in itself (see headers, and trailer_fields).
//
// NOTE: a message has a few fields, so they're kept in order (each name once), and looked up by going through them:
//       that, and holding the first few (and the first value of each) in itself, saves allocating for each one. Fields
//       removed (or cleared) are kept for their memory, which fields added later reuse: so headers reused for each
//       message on a connection (see clear()) needn't allocate for values that are too long to hold in themselves.
//       Its members are defined in headers.cpp, for the N of headers and trailer_fields only (see below).
template<std::size_t N>
class basic_headers
{
public:
    using values_type = header_values;
    using field_type  = header_field;

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

    basic_headers() = default;
    basic_headers(std::initializer_list<std::pair<std::string, std::vector<std::string>>> init);

    basic_headers(const basic_headers&)            = default;
    basic_headers& operator=(const basic_headers&) = default;

    basic_headers(basic_headers&& other) noexcept;
    basic_headers& operator=(basic_headers&& other) noexcept;

    ~basic_headers() = default;

    // set replaces key's values with val (or vals), and add adds val to them. Keys and values may be any kind of string
    // (e.g. set("Content-Type", "text/plain")); a std::string value is moved in, rather than copied.
    basic_headers& set(std::string_view key, std::string val);
    basic_headers& set(std::string_view key, std::string_view val);
    basic_headers& set(std::string_view key, const char* val) { return set(key, std::string_view{val}); }
    basic_headers& set(std::string_view key, std::initializer_list<std::string_view> vals);
    basic_headers& set(std::string_view key, std::vector<std::string>&& vals);

    basic_headers& add(std::string_view key, std::string val);
    basic_headers& add(std::string_view key, std::string_view val);
    basic_headers& add(std::string_view key, const char* val) { return add(key, std::string_view{val}); }

    // remove removes key's values, if it has any.
    basic_headers& remove(std::string_view key);

    // clear removes every field, keeping their memory for the fields added next.
    void clear() noexcept;

    basic_headers& set_content_length(std::size_t length);
    basic_headers& set_content_type(const content_type& content_type);

    // NOTE: the views get() and get_all() return are only valid until the headers are next changed (as a vector's
    //       elements' are), as the fields are held in order, rather than one by one.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const;
    [[nodiscard]] std::optional<std::string_view> operator[](std::string_view key) const;

    // get_all returns every value of key: one per field line, as they were received or set.
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

    // NOTE: defined here, so a fields_view is made from them without calls (e.g. for valid_fields()).
    [[nodiscard]] keys_iterator begin() const noexcept { return fields.data(); }
    [[nodiscard]] keys_iterator end() const noexcept { return fields.data() + used; }

    [[nodiscard]] bool        empty() const noexcept { return used == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return used; }

private:
    [[nodiscard]] const field_type* find(std::string_view key) const noexcept;
    [[nodiscard]] field_type*       find(std::string_view key) noexcept;

    // add_field adds key, with no values, reusing a removed field's memory if there is one. Its values must then be
    // set (with set_one, or after clearing them), as they may still be the removed field's.
    field_type& add_field(std::string_view key);

    // assign returns key's values, emptied, adding key if it has none.
    values_type& assign(std::string_view key);

    // set_one sets key's value to val, reusing the memory of its first value (or a removed field's).
    template<typename V>
    basic_headers& set_one(std::string_view key, V&& val);

    // fields holds the fields (the first used), and then those removed, kept for their memory.
    util::small_vector<field_type, N> fields;
    std::size_t                       used = 0;
};

// headers are a message's header fields (or a request's or response's head): holding 8 in themselves.
using headers = basic_headers<8>;

// trailer_fields are a message's trailer fields (RFC 9110, section 6.5), kept apart from its headers: holding 2 in
// themselves, as a message has none, or few (e.g. gRPC's grpc-status and grpc-message), and each request and response
// (and HTTP/2 stream with any) holds some. More are held as headers' past 8 are: allocated.
using trailer_fields = basic_headers<2>;

extern template class basic_headers<8>;
extern template class basic_headers<2>;

// same_fields is whether lhs and rhs have the same values for each name, in the same order, whatever order the names
// are in (and each name once, as basic_headers have them).
[[nodiscard]] bool same_fields(fields_view lhs, fields_view rhs) noexcept;

// basic_headers are equal if they have the same values for each name, in the same order, whatever order the names are
// in (see same_fields): however many each holds in itself.
template<std::size_t N, std::size_t M>
[[nodiscard]] bool operator==(const basic_headers<N>& lhs, const basic_headers<M>& rhs) noexcept
{
    return same_fields(lhs, rhs);
}

// is_valid_field_name checks that name is a token (RFC 9110, section 5.1).
[[nodiscard]] bool is_valid_field_name(std::string_view name) noexcept;

// is_valid_field_value checks that value has no control characters (CR, LF, NUL, ...) other than horizontal tab (RFC
// 9110, section 5.5).
[[nodiscard]] bool is_valid_field_value(std::string_view value) noexcept;

// valid_fields is whether every field's name and values are valid (as above), so that they can be sent without
// changing what's around them (e.g. a value with a line break, which would start another field).
[[nodiscard]] bool valid_fields(fields_view fields) noexcept;

// trailer_allowed is whether a field may be sent, or is kept when received, as a trailer (RFC 9110, section 6.5): not
// one that RFC 9110 (section 6.5.1) rules out, as Go's ValidTrailerHeader decides: framing, routing, authentication,
// request modifiers, response controls and content format (e.g. Content-Length, Host, Authorization, Cache-Control,
// Content-Type, Trailer, and every If-* field). Names are compared ignoring case.
[[nodiscard]] bool trailer_allowed(std::string_view name) noexcept;

// parse_field_line splits a field line ("name: value") into its name and value, without the whitespace around the
// value. It fails if the line is malformed, including obsolete line folding (a line starting with whitespace).
[[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>>
parse_field_line(std::string_view line) noexcept;

}
