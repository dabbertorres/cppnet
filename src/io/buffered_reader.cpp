#include "io/buffered_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <system_error>
#include <tuple>
#include <utility>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::io
{

buffered_reader::buffered_reader(reader* impl, std::size_t bufsize)
    : impl{impl}
    , buf(bufsize)
{}

coro::task<result> buffered_reader::read(std::span<std::byte> data)
{
    // usually, it's all buffered: then there's no need for a coroutine (see coro::ready_task).
    if (data.size() <= size())
    {
        std::ranges::copy(buffered().first(data.size()), data.begin());
        consume(data.size());

        return coro::ready_task<result>(result{.count = data.size()});
    }

    return read_more(data);
}

coro::task<result> buffered_reader::read_more(std::span<std::byte> data)
{
    // copy over what we can...
    std::size_t total = size();
    std::ranges::copy(buffered(), data.begin());
    consume(total);

    // At this point, the buffer is empty. As long as length - total is larger than the buffer capacity, we can skip a
    // copy and read straight to data: all that's left of it at once, so a large read (e.g. a WebSocket's message, or
    // a body) takes as few reads of what's wrapped as it can (each a syscall, or a coroutine).

    while (data.size() - total > capacity())
    {
        auto res = co_await impl->read(data.subspan(total));
        total += res.count;

        if (res.err) co_return result{.count = total, .err = res.err};
        if (res.count == 0) co_return result{.count = total, .err = make_error_code(errc::no_progress)};
    }

    // partial read of buffer capacity
    auto leftover = data.size() - total;
    if (leftover > 0)
    {
        co_await fill();

        // regardless of error, give the user what we can

        auto available = std::min(leftover, size());
        std::ranges::copy(buffered().first(available), data.subspan(total).begin());

        total += available;
        consume(available);

        // only report an error if we couldn't fulfill the caller's request
        if (available < leftover) co_return result{.count = total, .err = err};
    }

    co_return result{.count = total};
}

coro::task<buffered_reader::read_until_result> buffered_reader::read_until(std::span<const std::byte> delim) noexcept
{
    const auto data = std::span{buf}.subspan(start, size());

    // do we already have an instance of delim in the buffer?
    auto searcher    = std::boyer_moore_searcher{delim.begin(), delim.end()};
    auto delim_begin = std::search(data.begin(), data.end(), searcher);
    if (delim_begin != data.end())
    {
        co_return {
            .data      = {data.begin(), delim_begin},
            .is_prefix = false,
            .err       = err,
        };
    }

    co_return {
        .data      = data,
        .is_prefix = true,
        .err       = {},
    };
}

[[nodiscard]] coro::task<std::tuple<std::byte, bool>> buffered_reader::peek()
{
    using result_t = std::tuple<std::byte, bool>;

    if (size() == 0) co_await fill();

    if (size() == 0) co_return result_t{static_cast<std::byte>(0), false};
    co_return result_t{buf[start], true};
}

void buffered_reader::reset(reader* other)
{
    if (other != nullptr) impl = other;
    reset();
    err.clear();
}

void buffered_reader::reset()
{
    start = 0;
    end   = 0;
}

void buffered_reader::consume(std::size_t count) noexcept
{
    start += std::min(count, size());
    if (start == end) reset();
}

std::error_code buffered_reader::error() const { return err; }

coro::task<std::error_code> buffered_reader::fill()
{
    if (size() == capacity()) return coro::ready_task<std::error_code>();

    // make as much room as there can be.
    if (start != 0)
    {
        std::ranges::copy(buffered(), buf.begin());
        end -= start;
        start = 0;
    }

    // usually, the read doesn't wait (e.g. the data's there): then there's no need for a coroutine (see
    // coro::ready_task).
    auto reading = impl->read(std::span{buf}.subspan(end));
    if (reading.finished_without_coroutine())
        return coro::ready_task<std::error_code>(filled(std::move(reading).result()));

    return fill_after(std::move(reading));
}

coro::task<std::error_code> buffered_reader::fill_after(coro::task<result> reading)
{
    co_return filled(co_await std::move(reading));
}

std::error_code buffered_reader::filled(const result& res) noexcept
{
    end += res.count;

    // a reader that makes no progress without reporting why would otherwise leave callers waiting forever.
    err = res.count == 0 && !res.err ? make_error_code(errc::no_progress) : res.err;
    return err;
}
}
