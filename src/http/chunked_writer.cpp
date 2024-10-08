#include "http/chunked_writer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <span>
#include <string_view>

#include "coro/task.hpp"
#include "io/io.hpp"

// calculates the maximum decimal value that would fit in size digits.
// e.g. given 3, return 999.
consteval auto max_decimal_for_length_digits(auto size) noexcept
{
    if (size == 0) return 0;

    auto total = 0;

    for (auto i = 0; i < size; ++i)
    {
        total *= 10;
        total += 9;
    }

    return total;
}

namespace net::http::http11
{

using namespace std::string_view_literals;

coro::task<io::result> chunked_writer::write(std::span<const std::byte> data)
{
    std::array<char, 8>   chunk_size_buf{};
    constexpr std::size_t max_chunk_size = max_decimal_for_length_digits(chunk_size_buf.size());

    std::size_t amount_written = 0;

    while (amount_written < data.size())
    {
        std::size_t amount_to_write = std::min(max_chunk_size, data.size());

        // write the chunk length
        auto [ptr, ec] = std::to_chars(chunk_size_buf.begin(), chunk_size_buf.end(), amount_to_write);

        auto end = static_cast<std::size_t>(ptr - chunk_size_buf.begin());
        auto res = co_await parent->write(std::span{chunk_size_buf.data(), end});
        if (res.err) co_return {.count = amount_written, .err = res.err};

        res = co_await parent->write("\r\n"sv);
        if (res.err) co_return {.count = amount_written, .err = res.err};

        res = co_await parent->write(data.subspan(amount_written, amount_to_write));
        amount_written += res.count;
        if (res.err) co_return {.count = amount_written, .err = res.err};

        // end of chunk
        res = co_await parent->write("\r\n"sv);
        if (res.err) co_return {.count = amount_written, .err = res.err};
    }

    co_return {.count = amount_written};
}

}
