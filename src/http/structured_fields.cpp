#include "http/structured_fields.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "util/utf8.hpp"

namespace
{

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool is_lcalpha(char c) noexcept { return c >= 'a' && c <= 'z'; }
constexpr bool is_alpha(char c) noexcept { return is_lcalpha(c) || (c >= 'A' && c <= 'Z'); }

// is_tchar is whether c is one of a token's characters (RFC 9110, section 5.6.2).
constexpr bool is_tchar(char c) noexcept
{
    if (is_alpha(c) || is_digit(c)) return true;
    switch (c)
    {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~': return true;
    default: return false;
    }
}

// is_key_char is whether c may follow a key's first character (RFC 9651, section 3.1.2).
constexpr bool is_key_char(char c) noexcept
{
    return is_lcalpha(c) || is_digit(c) || c == '_' || c == '-' || c == '.' || c == '*';
}

// is_visible is whether c is in what a String or Display String may have unescaped: %x20-7E.
constexpr bool is_visible(char c) noexcept { return c >= 0x20 && c <= 0x7e; }

// lchex is a lowercase hex digit's value (as a Display String's percent-encoding must be), or -1.
constexpr int lchex(char c) noexcept
{
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// base64 is a base64 character's value (RFC 4648's alphabet: '+' and '/'), or -1.
constexpr int base64(char c) noexcept
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (is_lcalpha(c)) return c - 'a' + 26;
    if (is_digit(c)) return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

void skip_sp(std::string_view& in) noexcept
{
    while (!in.empty() && in.front() == ' ') in.remove_prefix(1);
}

void skip_ows(std::string_view& in) noexcept
{
    while (!in.empty() && (in.front() == ' ' || in.front() == '\t')) in.remove_prefix(1);
}

// read_digits reads the digits at the start of in into value, adding them to count: false if that's more than most.
bool read_digits(std::string_view& in, std::int64_t& value, std::size_t& count, std::size_t most) noexcept
{
    while (!in.empty() && is_digit(in.front()))
    {
        if (++count > most) return false;
        value = (value * 10) + (in.front() - '0');
        in.remove_prefix(1);
    }
    return true;
}

// prefix is in's first n characters (n being at most its size): as substr(), but without its check, which throws.
std::string_view prefix(std::string_view in, std::size_t n) noexcept
{
    return {in.begin(), in.begin() + static_cast<std::ptrdiff_t>(n)};
}

}

namespace net::http::sf::detail
{

// parser is RFC 9651's parsing algorithms (section 4.2). Each takes what's left of the value, in, from where it
// starts, and advances it past what it parsed, returning false if that's malformed (in then being anywhere).
class parser
{
public:
    // read_integer_or_decimal is section 4.2.4: an Integer of at most 15 digits, or a Decimal of at most 12 before
    // its '.' and from 1 to 3 after it.
    static bool read_integer_or_decimal(std::string_view& in, bare_item& out) noexcept
    {
        bool negative = false;
        if (!in.empty() && in.front() == '-')
        {
            negative = true;
            in.remove_prefix(1);
        }
        if (in.empty() || !is_digit(in.front())) return false;

        std::int64_t whole      = 0;
        std::size_t  whole_size = 0;
        if (!read_digits(in, whole, whole_size, 15)) return false;
        if (in.empty() || in.front() != '.')
        {
            out.type   = item_type::integer;
            out.number = negative ? -whole : whole;
            return true;
        }
        if (whole_size > 12) return false;
        in.remove_prefix(1);

        std::int64_t fraction      = 0;
        std::size_t  fraction_size = 0;
        if (!read_digits(in, fraction, fraction_size, 3) || fraction_size == 0) return false;
        for (std::size_t i = fraction_size; i < 3; ++i) fraction *= 10;
        const std::int64_t thousandths = (whole * 1'000) + fraction;
        out.type                       = item_type::decimal;
        out.number                     = negative ? -thousandths : thousandths;
        return true;
    }

    // read_string is section 4.2.5.
    static bool read_string(std::string_view& in, bare_item& out) noexcept
    {
        in.remove_prefix(1); // the '"'
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            const char c = in[i];
            if (c == '\\')
            {
                if (++i == in.size()) return false;
                if (in[i] != '"' && in[i] != '\\') return false;
            }
            else if (c == '"')
            {
                out.type = item_type::string;
                out.text = prefix(in, i);
                in.remove_prefix(i + 1);
                return true;
            }
            else if (!is_visible(c))
            {
                return false;
            }
        }
        return false; // no closing '"'
    }

    // read_token is section 4.2.6.
    static bool read_token(std::string_view& in, bare_item& out) noexcept
    {
        if (in.empty() || (!is_alpha(in.front()) && in.front() != '*')) return false;
        std::size_t i = 1;
        while (i < in.size() && (is_tchar(in[i]) || in[i] == ':' || in[i] == '/')) ++i;
        out.type = item_type::token;
        out.text = prefix(in, i);
        in.remove_prefix(i);
        return true;
    }

    // read_byte_sequence is section 4.2.7. Base64 without its padding, or with more than it needs, and with bits
    // set past its last byte, is accepted, as the RFC says parsers SHOULD NOT fail for them (it's how others encode
    // it); but padding only at its end, and not a length no bytes can have (one character past a multiple of 4).
    static bool read_byte_sequence(std::string_view& in, bare_item& out) noexcept
    {
        in.remove_prefix(1); // the ':'
        std::size_t data = 0;
        while (data < in.size() && base64(in[data]) >= 0) ++data;
        std::size_t end = data;
        while (end < in.size() && in[end] == '=') ++end;
        if (end == in.size() || in[end] != ':') return false;
        if (data % 4 == 1) return false;
        out.type = item_type::byte_sequence;
        out.text = prefix(in, end);
        in.remove_prefix(end + 1);
        return true;
    }

    // read_boolean is section 4.2.8.
    static bool read_boolean(std::string_view& in, bare_item& out) noexcept
    {
        in.remove_prefix(1); // the '?'
        if (in.empty() || (in.front() != '0' && in.front() != '1')) return false;
        out.type   = item_type::boolean;
        out.number = in.front() == '1' ? 1 : 0;
        in.remove_prefix(1);
        return true;
    }

    // read_date is section 4.2.9: an Integer after the '@'.
    static bool read_date(std::string_view& in, bare_item& out) noexcept
    {
        in.remove_prefix(1); // the '@'
        if (!read_integer_or_decimal(in, out) || out.type != item_type::integer) return false;
        out.type = item_type::date;
        return true;
    }

    // read_display_string is section 4.2.10.
    static bool read_display_string(std::string_view& in, bare_item& out) noexcept
    {
        if (in.size() < 2 || in[1] != '"') return false;
        in.remove_prefix(2); // the '%"'
        net::util::utf8_validator utf8;
        for (std::size_t i = 0; i < in.size(); ++i)
        {
            const char c = in[i];
            if (!is_visible(c)) return false;
            if (c == '%')
            {
                if (in.size() - i < 3) return false;
                const int high = lchex(in[i + 1]);
                const int low  = lchex(in[i + 2]);
                if (high < 0 || low < 0) return false;
                if (!utf8.feed(static_cast<std::byte>((high * 16) + low))) return false;
                i += 2;
            }
            else if (c == '"')
            {
                if (!utf8.complete()) return false;
                out.type = item_type::display_string;
                out.text = prefix(in, i);
                in.remove_prefix(i + 1);
                return true;
            }
            else if (!utf8.feed(static_cast<std::byte>(c)))
            {
                return false; // an ASCII character where the rest of one in UTF-8 should be
            }
        }
        return false; // no closing '"'
    }

    // read_bare_item is section 4.2.3.1.
    static bool read_bare_item(std::string_view& in, sf::bare_item& out) noexcept
    {
        out = {};
        if (in.empty()) return false;
        const char c = in.front();
        if (c == '-' || is_digit(c)) return read_integer_or_decimal(in, out);
        if (c == '"') return read_string(in, out);
        if (is_alpha(c) || c == '*') return read_token(in, out);
        if (c == ':') return read_byte_sequence(in, out);
        if (c == '?') return read_boolean(in, out);
        if (c == '@') return read_date(in, out);
        if (c == '%') return read_display_string(in, out);
        return false;
    }

    // read_key is section 4.2.3.3.
    static bool read_key(std::string_view& in, std::string_view& out) noexcept
    {
        if (in.empty() || (!is_lcalpha(in.front()) && in.front() != '*')) return false;
        std::size_t i = 1;
        while (i < in.size() && is_key_char(in[i])) ++i;
        out = prefix(in, i);
        in.remove_prefix(i);
        return true;
    }

    // read_parameter is one of section 4.2.3.2's: in starts with its ';'.
    static bool read_parameter(std::string_view& in, sf::parameter& out) noexcept
    {
        in.remove_prefix(1); // the ';'
        skip_sp(in);
        if (!read_key(in, out.key)) return false;
        out.value = {}; // true
        if (!in.empty() && in.front() == '=')
        {
            in.remove_prefix(1);
            return read_bare_item(in, out.value);
        }
        return true;
    }

    // read_parameters is section 4.2.3.2.
    static bool read_parameters(std::string_view& in, sf::parameters& out) noexcept
    {
        const std::string_view start = in;
        sf::parameter          p;
        while (!in.empty() && in.front() == ';')
        {
            if (!read_parameter(in, p)) return false;
        }
        out = sf::parameters{prefix(start, start.size() - in.size())};
        return true;
    }

    // read_item is section 4.2.3.
    static bool read_item(std::string_view& in, sf::item& out) noexcept
    {
        return read_bare_item(in, out.value) && read_parameters(in, out.params);
    }

    // read_inner_list is section 4.2.1.2: in starts with its '('.
    static bool read_inner_list(std::string_view& in, sf::inner_list& out) noexcept
    {
        in.remove_prefix(1); // the '('
        const std::string_view start = in;
        sf::item               i;
        while (!in.empty())
        {
            skip_sp(in);
            if (in.empty()) break;
            if (in.front() == ')')
            {
                const std::string_view items = prefix(start, start.size() - in.size());
                in.remove_prefix(1);
                sf::parameters params;
                if (!read_parameters(in, params)) return false;
                out = sf::inner_list{items, params};
                return true;
            }
            if (!read_item(in, i)) return false;
            if (in.empty() || (in.front() != ' ' && in.front() != ')')) return false;
        }
        return false; // no closing ')'
    }

    // read_item_or_inner_list is section 4.2.1.1.
    static bool read_item_or_inner_list(std::string_view& in, member& out) noexcept
    {
        // NOTE: a member assigned whole, then found with get_if, rather than emplace(): libstdc++'s emplace() (and
        //       assigning an alternative) returns through std::get, which may throw bad_variant_access. Its
        //       alternatives being trivially copyable, so is the variant's assignment.
        if (!in.empty() && in.front() == '(')
        {
            out = member{std::in_place_type<sf::inner_list>};
            return read_inner_list(in, *std::get_if<sf::inner_list>(&out));
        }
        out = member{std::in_place_type<sf::item>};
        return read_item(in, *std::get_if<sf::item>(&out));
    }

    // read_members parses a List's or a Dictionary's members (sections 4.2.1 and 4.2.2: the same but for the keys),
    // giving each to on_member, after the value's leading spaces.
    template<typename F>
    static bool read_members(std::string_view in, F on_member)
    {
        skip_sp(in);
        while (!in.empty())
        {
            if (!on_member(in)) return false;
            skip_ows(in);
            if (in.empty()) return true;
            if (in.front() != ',') return false;
            in.remove_prefix(1);
            skip_ows(in);
            if (in.empty()) return false; // a trailing comma
        }
        return true;
    }
};

bool next_parameter(std::string_view& rest, parameter& out) noexcept
{
    return !rest.empty() && parser::read_parameter(rest, out);
}

bool next_item(std::string_view& rest, item& out) noexcept
{
    skip_sp(rest);
    return !rest.empty() && parser::read_item(rest, out);
}

std::expected<void, std::error_code> parse_list(std::string_view value, list_callback on_member, void* context)
{
    member     m;
    const auto read_member = [&](std::string_view& in)
    {
        if (!parser::read_item_or_inner_list(in, m)) return false;
        on_member(context, m);
        return true;
    };
    if (!parser::read_members(value, read_member))
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    return {};
}

std::expected<void, std::error_code>
parse_dictionary(std::string_view value, dictionary_callback on_member, void* context)
{
    member     m;
    const auto read_member = [&](std::string_view& in)
    {
        std::string_view key;
        if (!parser::read_key(in, key)) return false;
        if (!in.empty() && in.front() == '=')
        {
            in.remove_prefix(1);
            if (!parser::read_item_or_inner_list(in, m)) return false;
        }
        else
        {
            // No value: Boolean true, with any parameters.
            auto& i = m.emplace<item>();
            if (!parser::read_parameters(in, i.params)) return false;
        }
        on_member(context, key, m);
        return true;
    };
    if (!parser::read_members(value, read_member))
        return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    return {};
}

}

namespace net::http::sf
{

std::expected<item, std::error_code> parse_item(std::string_view value) noexcept
{
    skip_sp(value);
    item i;
    if (!detail::parser::read_item(value, i)) return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    skip_sp(value);
    if (!value.empty()) return std::unexpected{std::make_error_code(std::errc::invalid_argument)};
    return i;
}

std::optional<bare_item> parameters::find(std::string_view key) const noexcept
{
    std::optional<bare_item> found;
    for (const auto& p : *this)
    {
        if (p.key == key) found = p.value;
    }
    return found;
}

void decode(const bare_item& item, std::string& out)
{
    const std::string_view text = item.text;
    switch (item.type)
    {
    case item_type::token: out += text; break;
    case item_type::string:
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\\') ++i; // the parser checked it's followed by '"' or '\'
            out += text[i];
        }
        break;
    case item_type::display_string:
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '%')
            {
                out += static_cast<char>((lchex(text[i + 1]) * 16) + lchex(text[i + 2]));
                i += 2;
            }
            else
            {
                out += text[i];
            }
        }
        break;
    case item_type::byte_sequence:
    {
        // Each character is 6 bits; whole bytes are taken as they're made, so what's left over at the end (the
        // padding's bits) is dropped.
        std::uint32_t bits  = 0;
        int           count = 0;
        for (const char c : text)
        {
            const int v = base64(c);
            if (v < 0) break; // the padding
            bits = (bits << 6) | static_cast<std::uint32_t>(v);
            count += 6;
            if (count >= 8)
            {
                count -= 8;
                out += static_cast<char>((bits >> count) & 0xff);
            }
        }
        break;
    }
    case item_type::integer:
    case item_type::decimal:
    case item_type::boolean:
    case item_type::date: break;
    }
}

std::string_view join_lines(std::span<const std::string> lines, std::string& buffer)
{
    if (lines.empty()) return {};
    if (lines.size() == 1) return lines.front();
    buffer.clear();
    for (const auto& line : lines)
    {
        if (&line != lines.data()) buffer += ", ";
        buffer += line;
    }
    return buffer;
}

}
