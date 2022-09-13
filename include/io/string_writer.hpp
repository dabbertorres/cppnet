#pragma once

#include <deque>
#include <ios>
#include <memory>
#include <string>

#include "io/writer.hpp"

namespace net::io
{

template<typename CharT, typename Traits = std::char_traits<CharT>, typename Allocator = std::allocator<CharT>>
class string_writer : public writer<CharT>
{
public:
    using string = std::basic_string<CharT, Traits, Allocator>;

    result write(const CharT* data, size_t length) override
    {
        parts.emplace_back(data, length);
        return {.count = length};
    }

    void write(CharT c)
    {
        if (parts.empty()) parts.emplace_back(1, c);
        else
        {
            auto& back = parts.back();
            if (back.size() < back.max_sie()) back.push_back(c);
            else parts.emplace_back(1, c);
        }
    }

    [[nodiscard]] string build() const
    {
        string output;
        output.reserve(length());

        for (const auto& p : parts)
        {
            output.append(p);
        }

        return output;
    }

    [[nodiscard]] size_t length() const
    {
        size_t total = 0;
        for (const auto& p : parts)
        {
            total += p.length();
        }
        return total;
    }

    void reset() { parts.clear(); }

private:
    std::deque<string> parts;
};

}
