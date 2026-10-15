#include "http/chunked_writer.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"

namespace
{

using namespace std::string_view_literals;

// write_full writes all of data, as a chunk's size has already promised it.
net::coro::task<net::io::result> write_full(net::io::writer* out, std::span<const std::byte> data)
{
    std::size_t written = 0;

    while (written < data.size())
    {
        auto res = co_await out->write(data.subspan(written));
        written += res.count;
        if (res.err) co_return {.count = written, .err = res.err};
    }

    co_return {.count = written};
}

net::coro::task<net::io::result> write_full(net::io::writer* out, std::string_view data)
{
    co_return co_await write_full(out, std::as_bytes(std::span{data}));
}

}

namespace net::http::http11
{

coro::task<io::result> chunked_writer::write(std::span<const std::byte> data)
{
    if (data.empty()) co_return {.count = 0};

    // the size, in hex, and its line ending.
    std::array<char, (std::numeric_limits<std::size_t>::digits / 4) + 2> size_line{};

    auto [end, _] = std::to_chars(size_line.data(), size_line.data() + size_line.size() - 2, data.size(), 16);
    *end++        = '\r';
    *end++        = '\n';

    auto res = co_await write_full(parent, std::string_view{size_line.data(), end});
    if (res.err) co_return {.err = res.err};

    res = co_await write_full(parent, data);
    if (res.err) co_return res;

    auto count = res.count;

    res = co_await write_full(parent, "\r\n"sv);
    if (res.err) co_return {.count = count, .err = res.err};

    co_return {.count = count};
}

coro::task<io::result> chunked_writer::finish()
{
    // the last chunk, and no trailers.
    auto res = co_await write_full(parent, "0\r\n\r\n"sv);
    co_return {.err = res.err};
}

}
