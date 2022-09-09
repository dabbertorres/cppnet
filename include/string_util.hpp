#pragma once

#include <coroutine>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

#include "coroutines.hpp"

namespace net::util
{

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string_view<CharT, Traits>>
generator<String> split_string(String input, CharT sep, typename String::size_type start = 0)
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
    return std::equal(std::begin(lhs),
                      std::end(lhs),
                      std::begin(rhs),
                      std::end(rhs),
                      [](unsigned char l, unsigned char r) { return std::tolower(l) == std::tolower(r); });
}

template<typename CharT  = char,
         typename Traits = std::char_traits<CharT>,
         typename String = std::basic_string_view<CharT, Traits>>
constexpr auto trim_string(String str) noexcept
{
    using namespace std::literals::string_view_literals;

    // source https://en.wikipedia.org/wiki/Whitespace_character#Unicode
    constexpr auto whitespace =
        " \b\f\n\r\t\v\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200A\u2028\u2029\u202f\u205f\u3000"sv;

    auto first_not_space = str.find_first_not_of(whitespace);
    auto last_not_space  = str.find_last_not_of(whitespace);

    if (last_not_space != String::npos) str.remove_suffix(str.size() - (last_not_space + 1));
    if (first_not_space != String::npos) str.remove_prefix(first_not_space);

    return str;
}

}
