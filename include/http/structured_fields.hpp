#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

// Structured Fields (RFC 9651): the syntax newer HTTP fields' values are written in (e.g. Priority's, RFC 9218): Lists,
// Dictionaries and Items, of Integers, Decimals, Strings, Tokens, Byte Sequences, Booleans, Dates and Display Strings,
// with parameters. Only parsing, for the fields the library reads itself (see docs/plans/http2.md, Priorities): it's
// what those are built on (e.g. parse_priority(), in http/priority.hpp), not for applications. sf, as the RFC's
// grammar names its rules (sf-list, sf-dictionary, sf-item).
//
// Parsing reports what it finds as views of the field's value, so nothing's allocated: a member's item as its type and
// its text or number, and its parameters, and an inner list's items, as views of their text, which the parser has
// checked and iterating reads again. So the field's value must outlive what's reported. A value that doesn't parse
// fails whole, with std::errc::invalid_argument: RFC 9651 has a field whose value fails be ignored whole.
namespace net::http::sf
{

// item_type is a bare item's type (RFC 9651, section 3.3).
enum class item_type : std::uint8_t
{
    integer,
    decimal,
    string,
    token,
    byte_sequence,
    boolean,
    date,
    display_string,
};

// bare_item is a value (RFC 9651, section 3.3), without its parameters: a number (an Integer, Decimal, Boolean or Date)
// or text (a String, Token, Byte Sequence or Display String).
struct bare_item
{
    item_type type = item_type::boolean;

    // number is an Integer's value (at most 15 digits, so within ±999,999,999,999,999); a Decimal's in thousandths,
    // which is exact (a Decimal has at most 12 integer digits and 3 fractional ones); a Boolean's, 0 or 1; or a Date's,
    // in seconds since the Unix epoch.
    std::int64_t number = 1;

    // text is a String's, Token's, Byte Sequence's or Display String's text, as the field has it, without what
    // delimits it: a String's escapes (\" and \\) are still in it, a Byte Sequence is base64 (perhaps without its
    // padding), and a Display String is percent-encoded UTF-8. So a Token's is its value, as is a String's that has no
    // backslash; decode() gives any of them.
    std::string_view text;

    // Each is the value, if the item is of that type.
    [[nodiscard]] std::optional<std::int64_t> integer() const noexcept
    {
        if (type == item_type::integer) return number;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<double> decimal() const noexcept
    {
        if (type == item_type::decimal) return static_cast<double>(number) / 1'000;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<bool> boolean() const noexcept
    {
        if (type == item_type::boolean) return number != 0;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::int64_t> date() const noexcept
    {
        if (type == item_type::date) return number;
        return std::nullopt;
    }
};

// decode appends a String's, Token's or Display String's value (UTF-8, which the parser checked), or a Byte Sequence's
// bytes, to out. For the other types, it appends nothing.
void decode(const bare_item& item, std::string& out);

// parameter is a parameter (RFC 9651, section 3.1.2): a key (lowercase), and its value (Boolean true if it has none).
struct parameter
{
    std::string_view key;
    bare_item        value;
};

struct item;

namespace detail
{
    class parser;

    // next_parameter and next_item read the next of what the parser has checked (a parameters' or an inner_list's text)
    // from the start of rest, advancing past it: false at its end.
    [[nodiscard]] bool next_parameter(std::string_view& rest, parameter& out) noexcept;
    [[nodiscard]] bool next_item(std::string_view& rest, item& out) noexcept;

    // view_iterator goes through a parameters' or inner_list's text, reading each of its T with Next as it gets to it.
    template<typename T, bool (*Next)(std::string_view&, T&) noexcept>
    class view_iterator
    {
    public:
        using value_type      = T;
        using difference_type = std::ptrdiff_t;

        view_iterator() = default;

        explicit view_iterator(std::string_view text) noexcept
            : rest{text}
        {
            ++*this;
        }

        [[nodiscard]] const T& operator*() const noexcept { return current; }
        [[nodiscard]] const T* operator->() const noexcept { return &current; }

        view_iterator& operator++() noexcept
        {
            done = !Next(rest, current);
            return *this;
        }

        void operator++(int) noexcept { ++*this; }

        [[nodiscard]] bool operator==(std::default_sentinel_t /*end*/) const noexcept { return done; }

    private:
        std::string_view rest;
        T                current{};
        bool             done = true;
    };
}

// parameters are an item's or inner list's parameters, in the order the field has them: a view of their text (e.g.
// ";a=1;b"), which iterating reads. A key may come more than once: RFC 9651 has a later one replace an earlier one
// (keeping the earlier's place), which find() does.
class parameters
{
public:
    using iterator = detail::view_iterator<parameter, detail::next_parameter>;

    parameters() = default;

    [[nodiscard]] iterator                begin() const noexcept { return iterator{checked}; }
    [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }

    [[nodiscard]] bool empty() const noexcept { return checked.empty(); }

    // find is the value of the last parameter named key, if there is one.
    [[nodiscard]] std::optional<bare_item> find(std::string_view key) const noexcept;

    // text is the parameters' text, as the field has it.
    [[nodiscard]] std::string_view text() const noexcept { return checked; }

private:
    friend class detail::parser;

    explicit parameters(std::string_view text) noexcept
        : checked{text}
    {}

    std::string_view checked;
};

// item is an Item (RFC 9651, section 3.3): a bare item, and its parameters.
struct item
{
    bare_item  value;
    parameters params;
};

// inner_list is an Inner List (RFC 9651, section 3.1.1): items, in order, as a view of their text (between the
// parentheses), which iterating reads; and the list's own parameters.
class inner_list
{
public:
    using iterator = detail::view_iterator<item, detail::next_item>;

    inner_list() = default;

    [[nodiscard]] iterator                begin() const noexcept { return iterator{checked}; }
    [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }

    // empty is whether it has no items.
    [[nodiscard]] bool empty() const noexcept { return begin() == end(); }

    [[nodiscard]] const parameters& params() const noexcept { return own; }

private:
    friend class detail::parser;

    inner_list(std::string_view items, parameters params) noexcept
        : checked{items}
        , own{params}
    {}

    std::string_view checked;
    parameters       own;
};

// member is a List's or Dictionary's member: an Item or an Inner List.
using member = std::variant<item, inner_list>;

namespace detail
{
    using list_callback       = void (*)(void* context, const member& m);
    using dictionary_callback = void (*)(void* context, std::string_view key, const member& m);

    std::expected<void, std::error_code> parse_list(std::string_view value, list_callback on_member, void* context);
    std::expected<void, std::error_code>
    parse_dictionary(std::string_view value, dictionary_callback on_member, void* context);
}

// parse_item parses a field's value as an Item (RFC 9651, section 4.2.3), failing with std::errc::invalid_argument if
// it isn't one. Leading and trailing spaces are allowed, as the RFC allows (but no other whitespace).
[[nodiscard]] std::expected<item, std::error_code> parse_item(std::string_view value) noexcept;

// parse_list parses a field's value as a List (RFC 9651, section 4.2.1), calling on_member(const member&) for each
// member, in order, as it's parsed: so if parsing then fails (std::errc::invalid_argument), on_member has seen the
// members before what's malformed, and whatever it made of them must be dropped, as the field is ignored whole. An
// empty value is an empty List. value is the field's lines joined (see join_lines()).
template<typename F>
std::expected<void, std::error_code> parse_list(std::string_view value, F on_member)
{
    return detail::parse_list(
        value,
        [](void* context, const member& m) { (*static_cast<F*>(context))(m); },
        static_cast<void*>(&on_member));
}

// parse_dictionary parses a field's value as a Dictionary (RFC 9651, section 4.2.2), calling on_member(std::string_view
// key, const member&) for each member, in order, as parse_list() does (and as it says, failing whole). A key may come
// more than once: each is reported, and RFC 9651 has the later replace the earlier (keeping the earlier's place), which
// is on_member's to do. A member without a value (e.g. "i") is Boolean true, with any parameters it has.
template<typename F>
std::expected<void, std::error_code> parse_dictionary(std::string_view value, F on_member)
{
    return detail::parse_dictionary(
        value,
        [](void* context, std::string_view key, const member& m) { (*static_cast<F*>(context))(key, m); },
        static_cast<void*>(&on_member));
}

// join_lines is a field's value as Structured Fields are parsed from it (RFC 9651, section 4.2): its field lines (e.g.
// a headers' get_all(), one value a line), joined with ", ", empty ones too (so an empty line in a List is an empty
// member, which fails). One line is returned as it is, without copying, and none is ""; several are joined into buffer
// (replacing what it held), of which the result is a view.
[[nodiscard]] std::string_view join_lines(std::span<const std::string> lines, std::string& buffer);

}
