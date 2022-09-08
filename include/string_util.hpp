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

}
