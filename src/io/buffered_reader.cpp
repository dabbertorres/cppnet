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

namespace
{

// busy_error is what a read or fill that finds another waiting makes.
std::error_code busy_error() noexcept { return std::make_error_code(std::errc::device_or_resource_busy); }

}

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
    // it takes what's buffered, and reads the rest: without a coroutine, unless a read has to wait, which reading
    // finishes (see read_on()).
    return reading.adapt(
        [this, data]
        {
            got = size();
            std::ranges::copy(buffered(), data.begin());
            consume(got);
            wanted = data;
            return read_rest();
        },
        [] { return result{.err = busy_error()}; });
}

coro::step<result, result> buffered_reader::read_rest()
{
    // the buffer's empty: as long as what's left is more than it holds, a copy's skipped by reading straight into
    // wanted, all that's left of it at once, so a large read (e.g. a WebSocket's message, or a body) takes as few reads
    // of what's wrapped as it can (each a syscall, or a coroutine).
    const auto left = wanted.size() - got;
    direct          = left > capacity();
    if (direct) return impl->read(wanted.subspan(got));
    if (left == 0) return result{.count = got};

    // the rest, through the buffer (as fill() would read it).
    return impl->read(std::span{buf}.subspan(end));
}

coro::step<result, result> buffered_reader::read_on(result res)
{
    if (direct)
    {
        got += res.count;
        if (res.err) return result{.count = got, .err = res.err};
        if (res.count == 0) return result{.count = got, .err = make_error_code(errc::no_progress)};
        return read_rest();
    }

    // regardless of error, give the caller what we can: an error's reported only if it can't have all it asked for.
    const auto failed    = filled(res);
    const auto left      = wanted.size() - got;
    const auto available = std::min(left, size());
    std::ranges::copy(buffered().first(available), wanted.subspan(got).begin());
    got += available;
    consume(available);

    if (available < left) return result{.count = got, .err = failed};
    return result{.count = got};
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

    // usually, the read doesn't wait (e.g. the data's there): then it's finished at once. One that waits is the
    // adapter's, which finishes it with filled() when it's awaited, rather than a coroutine's.
    return filling.adapt(
        [this]
        {
            // make as much room as there can be.
            if (start != 0)
            {
                std::ranges::copy(buffered(), buf.begin());
                end -= start;
                start = 0;
            }
            return impl->read(std::span{buf}.subspan(end));
        },
        &busy_error);
}

std::error_code buffered_reader::filled(const result& res) noexcept
{
    end += res.count;

    // a reader that makes no progress without reporting why would otherwise leave callers waiting forever.
    err = res.count == 0 && !res.err ? make_error_code(errc::no_progress) : res.err;
    return err;
}
}
