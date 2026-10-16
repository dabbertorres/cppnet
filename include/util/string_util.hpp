#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace net::util
{

// string_parts is the parts of a string, split at each separator (see split_string).
template<typename String, typename CharT>
class string_parts
{
public:
    using size_type = String::size_type;

    class iterator
    {
    public:
        using value_type      = String;
        using difference_type = std::ptrdiff_t;

        constexpr iterator() noexcept = default;

        constexpr String operator*() const { return parts->input.substr(begin, end - begin); }

        constexpr iterator& operator++() noexcept
        {
            // after the last part (which ends at the end of the input, rather than at a separator), there are none.
            if (end == parts->input.length())
            {
                begin = String::npos;
            }
            else
            {
                begin = end + 1;
                end   = parts->part_end(begin);
            }

            return *this;
        }

        constexpr void operator++(int) noexcept { ++*this; }

        friend constexpr bool operator==(const iterator& it, std::default_sentinel_t /*unused*/) noexcept
        {
            return it.begin == String::npos;
        }

    private:
        friend class string_parts;

        constexpr iterator(const string_parts* parts, size_type begin) noexcept
            : parts{parts}
            , begin{begin}
            , end{begin == String::npos ? String::npos : parts->part_end(begin)}
        {}

        const string_parts* parts = nullptr;
        size_type           begin = String::npos;
        size_type           end   = String::npos;
    };

    constexpr string_parts(String    input,
                           CharT     sep,
                           size_type start) noexcept(std::is_nothrow_move_constructible_v<String>)
        : input{std::move(input)}
        , sep{sep}
        , start{start}
    {}

    // NOTE: iterators refer to this, so it must outlive them (as it does in a range-based for loop).
    [[nodiscard]] constexpr iterator begin() const noexcept
    {
        return {this, start <= input.length() ? start : String::npos};
    }

    [[nodiscard]] constexpr std::default_sentinel_t end() const noexcept { return {}; }

private:
    [[nodiscard]] constexpr size_type part_end(size_type from) const noexcept
    {
        auto found = input.find(sep, from);
        return found == String::npos ? input.length() : found;
    }

    String    input;
    CharT     sep;
    size_type start;
};

// split_string splits input, from start, at each sep: e.g. "a,,b" into "a", "", and "b", and "a," into "a" and "".
// Unlike std::views::split, an empty input is one (empty) part, rather than none.
template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string_view<CharT, Traits>>
constexpr string_parts<String, CharT> split_string(String input, CharT sep, typename String::size_type start = 0)
{
    return {std::move(input), sep, start};
}

// to_lower_ascii lowercases c if it's an ASCII letter. Unlike std::tolower, it doesn't depend on the locale (or call
// into the C library), as the case-insensitive names that protocols have (e.g. HTTP's field names) are ASCII.
template<typename CharT>
constexpr CharT to_lower_ascii(CharT c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<CharT>(c - 'A' + 'a') : c;
}

// equal_ignore_case compares lhs and rhs, ignoring the case of ASCII letters.
template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string_view<CharT, Traits>>
constexpr bool equal_ignore_case(String lhs, String rhs) noexcept
{
    constexpr auto compare = [](CharT l, CharT r) { return to_lower_ascii(l) == to_lower_ascii(r); };
    return std::equal(std::begin(lhs), std::end(lhs), std::begin(rhs), std::end(rhs), compare);
}

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string_view<CharT, Traits>>
constexpr bool less_ignore_case(String lhs, String rhs) noexcept
{
    constexpr auto compare = [](CharT l, CharT r) { return to_lower_ascii(l) < to_lower_ascii(r); };
    return std::lexicographical_compare(std::begin(lhs), std::end(lhs), std::begin(rhs), std::end(rhs), compare);
}

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string_view<CharT, Traits>>
// NOLINTNEXTLINE(bugprone-exception-escape): substr's positions are in range.
constexpr String trim_string(const String& str) noexcept
{
    using namespace std::literals::string_view_literals;

    // ASCII whitespace only: the bytes of a multi-byte (UTF-8) character would otherwise be taken for whitespace.
    constexpr auto whitespace = " \t\n\v\f\r"sv;

    const auto first = str.find_first_not_of(whitespace);
    if (first == String::npos) return str.substr(str.size()); // all whitespace

    const auto last = str.find_last_not_of(whitespace);
    return str.substr(first, last - first + 1);
}

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string<CharT, Traits>,
         typename View   = std::basic_string_view<CharT, Traits>,
         std::convertible_to<View> Args>
String join(Args join_with, std::initializer_list<Args> strings) noexcept
{
    switch (strings.size())
    {
    case 0: return "";
    case 1: return String{*strings.begin()};
    }

    std::size_t total = 0;
    for (const auto& s : strings) total += s.size();

    String out;
    out.reserve(total);

    auto begin = strings.begin();
    auto end   = strings.end();

    out.append(*begin);
    ++begin;

    for (auto it = begin; it != end; ++it)
    {
        out.append(join_with);
        out.append(*it);
    }

    return out;
}

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string_view<CharT, Traits>>
// NOLINTNEXTLINE(bugprone-exception-escape): substr's positions are in range.
constexpr String unquote(String str) noexcept
{
    // need at least 2 characters to be quoted
    if (str.size() < 3) return str;

    auto first = str.front();

    if (first != '"' && first != '\'') return str; // not quoted
    if (first != str.back()) return str;           // not the same quote

    return str.substr(1, str.size() - 2);
}

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string<CharT, Traits>,
         typename View   = std::basic_string_view<CharT, Traits>>
constexpr String replace(View view, View replace, View with_this)
{
    std::basic_stringstream<CharT, Traits> ss;

    while (!view.empty())
    {
        if (view.starts_with(replace))
        {
            ss << with_this;
            view = view.substr(replace.length());
        }
        else
        {
            ss << view.front();
            view = view.substr(1);
        }
    }

    return ss.str();
}

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string<CharT, Traits>,
         typename View   = std::basic_string_view<CharT, Traits>>
constexpr void replace_all_to(std::ostream& os, View view, std::initializer_list<std::pair<View, View>> replacements)
{
    while (!view.empty())
    {
        bool replaced = false;
        for (const auto& [replace, with_this] : replacements)
        {
            if (view.starts_with(replace))
            {
                os << with_this;
                view     = view.substr(replace.length());
                replaced = true;
                break;
            }
        }

        if (!replaced)
        {
            os << view.front();
            view = view.substr(1);
        }
    }
}

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string<CharT, Traits>,
         typename View   = std::basic_string_view<CharT, Traits>>
constexpr String replace_all(View view, std::initializer_list<std::pair<View, View>> replacements)
{
    std::basic_stringstream<CharT, Traits> ss;
    replace_all_to(ss, view, replacements);
    return ss.str();
}

}
