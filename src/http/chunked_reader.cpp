#include "http/chunked_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "http/headers.hpp"
#include "http/http11.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/util.hpp"

namespace
{

std::error_code malformed() noexcept { return std::make_error_code(std::errc::illegal_byte_sequence); }

// truncated is for a body that the end of the stream cuts short: that's not the end of the body.
std::error_code truncated(std::error_code err) noexcept
{
    if (err == net::io::errc::end_of_stream) return std::make_error_code(std::errc::connection_aborted);
    return err;
}

int hex_digit_value(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}

namespace net::http::http11
{

coro::task<io::result> chunked_reader::read(std::span<std::byte> data)
{
    if (data.empty()) co_return {.count = 0};

    // don't read past the end of the body, into whatever follows it.
    if (done) co_return {.count = 0, .err = make_error_code(io::errc::end_of_stream)};

    if (at_start) [[unlikely]]
    {
        const auto callback = std::exchange(at_start, {});
        if (auto err = co_await callback.call(callback.context); err) co_return {.err = err};
    }

    if (chunk_remaining == 0)
    {
        if (after_chunk)
        {
            if (auto err = co_await read_crlf(); err) co_return {.err = err};
            after_chunk = false;
        }

        if (auto err = co_await read_chunk_size(); err) co_return {.err = err};

        // the last chunk is empty, and followed by the trailers.
        if (chunk_remaining == 0)
        {
            if (auto err = co_await read_trailers(); err) co_return {.err = err};

            done = true;
            at_end();
            co_return {.count = 0, .err = make_error_code(io::errc::end_of_stream)};
        }
    }

    auto res = co_await parent->read(data.first(std::min(data.size(), chunk_remaining)));
    chunk_remaining -= res.count;
    if (chunk_remaining == 0) after_chunk = true;

    if (res.err) co_return {.count = res.count, .err = truncated(res.err)};
    co_return {.count = res.count};
}

coro::task<std::error_code> chunked_reader::read_chunk_size()
{
    auto read = co_await io::read_terminated_line(parent, max_size_line);
    if (!read.has_value()) co_return truncated(read.error());
    const auto& line = read.value();

    // chunk-size = 1*HEXDIG
    std::size_t size   = 0;
    std::size_t digits = 0;
    for (; digits < line.size(); ++digits)
    {
        auto value = hex_digit_value(line[digits]);
        if (value < 0) break;

        if (size > (std::numeric_limits<std::size_t>::max() >> 4U)) co_return malformed();
        size = (size << 4U) | static_cast<std::size_t>(value);
    }

    if (digits == 0) co_return malformed();

    // chunk-ext = *( BWS ";" BWS chunk-ext-name [ BWS "=" BWS chunk-ext-val ] ), which are skipped.
    std::string_view rest{line};
    rest.remove_prefix(digits);

    // (whitespace is only allowed before a ';'.)
    if (!rest.empty())
    {
        auto ext_start = rest.find_first_not_of(" \t");
        if (ext_start == std::string_view::npos || rest[ext_start] != ';' || !is_valid_field_value(rest))
            co_return malformed();
    }

    chunk_remaining = size;
    co_return std::error_code{};
}

coro::task<std::error_code> chunked_reader::read_trailers()
{
    if (kept != nullptr) kept->clear();

    // a section that fails leaves no part of itself.
    auto failed = [this](std::error_code err)
    {
        if (kept != nullptr) kept->clear();
        return err;
    };

    std::size_t remaining = max_trailers;

    for (std::size_t count = 0;; ++count)
    {
        auto read = co_await io::read_terminated_line(parent, remaining);
        if (!read.has_value()) co_return failed(truncated(read.error()));
        const auto& line = read.value();
        remaining -= line.size() + 2;

        if (line.empty()) co_return std::error_code{};

        // as a head's field lines are limited (see http11::max_fields), whether they're kept or not, so a body decodes
        // the same either way.
        if (count == max_fields) co_return failed(std::make_error_code(std::errc::value_too_large));

        auto field = parse_field_line(line);
        if (!field.has_value()) co_return failed(malformed());

        // fields that can't be trailers are dropped, rather than failing the body (and are never taken for the
        // header's, e.g. a Content-Length).
        if (kept == nullptr || !trailer_allowed(field->first)) continue;

        try
        {
            kept->add(field->first, field->second);
        }
        catch (const std::bad_alloc&)
        {
            co_return failed(std::make_error_code(std::errc::not_enough_memory));
        }
    }
}

coro::task<std::error_code> chunked_reader::read_crlf()
{
    constexpr std::array crlf{'\r', '\n'};

    for (char expect : crlf)
    {
        char next = 0;
        auto res  = co_await parent->read(next);
        if (res.err) co_return truncated(res.err);
        if (res.count == 0) co_return truncated(make_error_code(io::errc::end_of_stream));
        if (next != expect) co_return malformed();
    }

    co_return std::error_code{};
}

}
