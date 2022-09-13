#pragma once

#include <algorithm>
#include <string_view>

#include "io/reader.hpp"

namespace net::io
{

template<typename CharT, typename Traits = std::char_traits<CharT>>
class string_reader : public reader<CharT>
{
public:
    using string = std::basic_string_view<CharT, Traits>;

    string_reader(string view)
        : view{view}
    {}

    result read(CharT* data, size_t length) override
    {
        auto amount = std::min(view.length() - idx, length);
        auto start  = view.begin() + idx;
        std::copy_n(start, amount, data);
        idx += amount;
        return {.count = amount};
    }

private:
    string view;
    size_t idx = 0;
};

}
