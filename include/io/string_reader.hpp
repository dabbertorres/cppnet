#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::io
{

template<typename CharT, typename Traits = std::char_traits<CharT>>
class string_reader : public reader
{
public:
    using string = std::basic_string_view<CharT, Traits>;

    string_reader(string view)
        : view{std::as_bytes(std::span{view})}
    {}

    coro::task<result> read(std::span<CharT> data)
    {
        auto [count, err] = co_await read(std::as_writable_bytes(data));
        co_return {
            .count = count / sizeof(CharT),
            .err   = err,
        };
    }

    coro::task<result> read(std::span<std::byte> data) override
    {
        if (idx == view.size()) co_return {.count = 0};

        auto amount = std::min(view.size() - idx, data.size());
        auto start  = view.begin() + idx;
        std::copy_n(start, amount, data.begin());
        idx += amount;
        co_return {.count = amount};
    }

    [[nodiscard]] int native_handle() const noexcept override { return 0; }

private:
    std::span<const std::byte> view;
    std::size_t                idx = 0;
};

}
