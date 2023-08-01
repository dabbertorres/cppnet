#pragma once

#include <algorithm>
#include <string_view>

#include "io/reader.hpp"

namespace net::io
{

template<typename CharT, typename Traits = std::char_traits<CharT>>
class string_reader : public reader
{
public:
    using string = std::basic_string_view<CharT, Traits>;

    string_reader(string view)
        : view{view}
    {}

    result read(CharT* data, std::size_t length)
    {
        if (idx == view.length()) return {.count = 0};

        auto amount = std::min(view.length() - idx, length);
        auto start  = view.begin() + idx;
        std::copy_n(start, amount, data);
        idx += amount;
        return {.count = amount};
    }

    result read(std::byte* data, std::size_t length) override { return read(reinterpret_cast<CharT*>(data), length); }

    [[nodiscard]] int native_handle() const noexcept override { return 0; }

private:
    string      view;
    std::size_t idx = 0;
};

}
