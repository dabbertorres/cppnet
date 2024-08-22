#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <span>
#include <string>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"

namespace net::io
{

template<typename CharT, typename Traits = std::char_traits<CharT>, typename Allocator = std::allocator<CharT>>
class string_writer : public writer
{
public:
    using string = std::basic_string<CharT, Traits, Allocator>;

    coro::task<result> write(std::span<const CharT> data)
    {
        parts.emplace_back(data.data(), data.size());
        co_return {.count = data.size()};
    }

    coro::task<result> write(std::span<const std::byte> data) override
    {
        parts.emplace_back(reinterpret_cast<const CharT*>(data.data()), data.size() / sizeof(CharT));
        co_return {.count = data.size()};
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

    [[nodiscard]] std::size_t length() const
    {
        std::size_t total = 0;
        for (const auto& p : parts)
        {
            total += p.length();
        }
        return total;
    }

    void reset() { parts.clear(); }

    [[nodiscard]] int native_handle() const noexcept override { return 0; }

private:
    std::deque<string> parts;
};

}
