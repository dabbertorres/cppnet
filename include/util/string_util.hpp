#pragma once

#include <cctype>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "coro/generator.hpp"
#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"

namespace net::util
{

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string_view<CharT, Traits>>
coro::generator<String> split_string(String input, CharT sep, typename String::size_type start = 0)
{
    for (auto i = start; i <= input.length(); ++i)
    {
        auto next = input.find(sep, i);

        if (next == String::npos)
        {
            co_yield input.substr(i);
            break;
        }

        co_yield input.substr(i, next - i);

        i = next;
    }
}

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string_view<CharT, Traits>>
constexpr bool equal_ignore_case(String lhs, String rhs) noexcept
{
    constexpr auto compare = [](unsigned char l, unsigned char r) { return std::tolower(l) == std::tolower(r); };
    return std::equal(std::begin(lhs), std::end(lhs), std::begin(rhs), std::end(rhs), compare);
}

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string_view<CharT, Traits>>
constexpr bool less_ignore_case(String lhs, String rhs) noexcept
{
    constexpr auto compare = [](unsigned char l, unsigned char r) { return std::tolower(l) < std::tolower(r); };
    return std::equal(std::begin(lhs), std::end(lhs), std::begin(rhs), std::end(rhs), compare);
}

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string_view<CharT, Traits>>
constexpr String trim_string(String str) noexcept
{
    using namespace std::literals::string_view_literals;

    // source https://en.wikipedia.org/wiki/Whitespace_character#Unicode
    constexpr auto whitespace =
        " \b\f\n\r\t\v\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200A\u2028\u2029\u202f\u205f\u3000"sv;

    std::size_t first_not_space = str.find_first_not_of(whitespace);
    std::size_t last_not_space  = str.find_last_not_of(whitespace);

    if (first_not_space == String::npos) first_not_space = 0;
    if (last_not_space == String::npos) last_not_space = str.length();

    last_not_space -= first_not_space - 1;

    return str.substr(first_not_space, last_not_space);
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
coro::task<io::result>
replace_all_to(io::writer& out, View view, std::initializer_list<std::pair<View, View>> replacements)
{
    std::size_t total = 0;

    while (!view.empty())
    {
        bool replaced = false;
        for (const auto& [replace, with_this] : replacements)
        {
            if (view.starts_with(replace))
            {
                auto res = co_await out.write(with_this);
                total += res.count;
                if (res.err) co_return {.count = total, .err = res.err};

                view     = view.substr(replace.length());
                replaced = true;
                break;
            }
        }

        if (!replaced)
        {
            auto res = co_await out.write(view.front());
            total += res.count;
            if (res.err) co_return {.count = total, .err = res.err};

            view = view.substr(1);
        }
    }

    co_return {.count = total};
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
