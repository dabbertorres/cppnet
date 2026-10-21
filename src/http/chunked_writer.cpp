#include "http/chunked_writer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "http/headers.hpp"
#include "io/buffered_writer.hpp"
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

// last_chunk_size is how long append_last_chunk makes the last chunk, with trailers' allowed fields.
std::size_t last_chunk_size(const net::http::headers* trailers) noexcept
{
    // "0\r\n", the field lines, then "\r\n".
    std::size_t size = 5;
    if (trailers == nullptr) return size;

    for (const auto& [name, values] : *trailers)
    {
        if (!net::http::trailer_allowed(name)) continue;
        for (const auto& value : values) size += name.size() + value.size() + 4;
    }

    return size;
}

// append_last_chunk appends the last chunk, with a field line for each value of trailers' allowed fields, to out (a
// string, or a span_appender).
void append_last_chunk(auto& out, const net::http::headers* trailers)
{
    out.append("0\r\n"sv);

    if (trailers != nullptr)
    {
        for (const auto& [name, values] : *trailers)
        {
            if (!net::http::trailer_allowed(name)) continue;
            for (const auto& value : values) out.append(name).append(": "sv).append(value).append("\r\n"sv);
        }
    }

    out.append("\r\n"sv);
}

// span_appender appends to a span, as std::string::append does to a string: the span must be long enough.
struct span_appender
{
    std::span<std::byte> out;
    std::size_t          size = 0;

    span_appender& append(std::string_view str) noexcept
    {
        std::ranges::copy(std::as_bytes(std::span{str}), out.subspan(size).begin());
        size += str.size();
        return *this;
    }
};

// write_last_chunk writes the last chunk, with trailers' allowed fields, when it can't be formatted into the writer's
// buffer (see finish()).
net::coro::task<net::io::result> write_last_chunk(net::io::writer* out, const net::http::headers* trailers)
{
    try
    {
        // NOTE: written all at once, rather than a write (and so, a coroutine) for each part of it.
        std::string chunk;
        chunk.reserve(last_chunk_size(trailers));
        append_last_chunk(chunk, trailers);

        auto res = co_await write_full(out, std::string_view{chunk});
        co_return {.err = res.err};
    }
    catch (const std::bad_alloc&)
    {
        co_return {.err = std::make_error_code(std::errc::not_enough_memory)};
    }
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

coro::task<io::result> chunked_writer::finish(const headers* trailers)
{
    if (parent == nullptr)
        return coro::ready_task<io::result>(io::result{.err = std::make_error_code(std::errc::invalid_argument)});

    // without trailers, it's just "0\r\n\r\n".
    if (trailers != nullptr && trailers->empty()) trailers = nullptr;

    // nothing's written if a trailer can't be, so the caller can cut the body short instead (rather than end it
    // without them, or with only some).
    if (trailers != nullptr && !valid_fields(*trailers))
        return coro::ready_task<io::result>(io::result{.err = std::make_error_code(std::errc::invalid_argument)});

    // usually, it's a connection's buffered writer, with room for it: then it's formatted straight into the buffer,
    // without a coroutine (see coro::ready_task).
    if (auto* buffered = dynamic_cast<io::buffered_writer*>(parent); buffered != nullptr)
    {
        try
        {
            if (auto out = buffered->prepare(last_chunk_size(trailers)); !out.empty())
            {
                span_appender appender{.out = out};
                append_last_chunk(appender, trailers);
                return coro::ready_task<io::result>();
            }
        }
        catch (const std::bad_alloc&)
        {
            return coro::ready_task<io::result>(io::result{.err = std::make_error_code(std::errc::not_enough_memory)});
        }
    }

    return write_last_chunk(parent, trailers);
}

}
