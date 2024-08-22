#include "io/buffered_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <span>
#include <system_error>
#include <tuple>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::io
{

buffered_reader::buffered_reader(reader* impl, std::size_t bufsize)
    : impl{impl}
    , buf{bufsize}
{
    buf.resize(0);
}

coro::task<result> buffered_reader::read(std::span<std::byte> data)
{
    if (data.empty()) co_return result{.count = 0};

    // easy way out
    if (data.size() <= buf.size())
    {
        std::copy_n(buf.begin(), data.size(), data.begin());

        auto end = buf.begin();
        std::advance(end, data.size());

        auto leftover = buf.size() - data.size();
        auto new_end  = buf.begin();
        std::advance(new_end, leftover);

        std::copy_backward(end, buf.end(), new_end);
        buf.resize(leftover);

        co_return result{.count = data.size()};
    }

    // Note that we always (try to) read from the inner reader in buf.capacity() increments.

    std::size_t total = 0;

    if (!buf.empty())
    {
        // copy over what we can...

        std::copy_n(buf.begin(), buf.size(), data.begin());
        total += buf.size();
        buf.resize(0);
    }

    // At this point, the buffer is empty. As long as length - total is larger
    // than the buffer capacity, we can skip a copy and read straight to data.

    while (data.size() - total > buf.capacity())
    {
        auto res = co_await impl->read(data.subspan(total, buf.capacity()));
        if (res.err) co_return result{.count = total + res.count, .err = res.err};

        total += res.count;
    }

    // partial read of buffer capacity
    auto leftover = data.size() - total;
    if (leftover > 0)
    {
        co_await fill();

        // regardless of error, give the user what we can

        auto available = std::min(leftover, buf.size());
        std::copy_n(buf.begin(), available, data.subspan(total).begin());

        total += available;

        auto end = buf.begin();
        std::advance(end, available);
        std::copy(end, buf.end(), buf.begin());

        buf.resize(buf.size() - available);

        // only report an error if we couldn't fulfill the caller's request
        if (available < leftover) co_return result{.count = total, .err = err};
    }

    co_return result{.count = total};
}

coro::task<buffered_reader::read_until_result> buffered_reader::read_until(std::span<const std::byte> delim) noexcept
{
    // do we already have an instance of delim in the buffer?
    auto searcher    = std::boyer_moore_searcher{delim.begin(), delim.end()};
    auto delim_begin = std::search(buf.begin(), buf.end(), searcher);
    if (delim_begin != buf.end())
    {
        co_return {
            .data      = {buf.begin(), delim_begin},
            .is_prefix = false,
            .err       = err,
        };
    }

    co_return {
        .data      = {buf.begin(), buf.end()},
        .is_prefix = true,
        .err       = {},
    };
}

[[nodiscard]] coro::task<std::tuple<std::byte, bool>> buffered_reader::peek()
{
    using result_t = std::tuple<std::byte, bool>;

    if (!buf.empty()) co_return result_t{buf.front(), true};
    co_await fill();

    if (buf.empty()) co_return result_t{static_cast<std::byte>(0), false};
    co_return result_t{buf.front(), true};
}

void buffered_reader::reset(reader* other)
{
    if (other != nullptr) impl = other;
    buf.resize(0);
    err.clear();
}

void buffered_reader::reset() { buf.resize(0); }

std::error_condition buffered_reader::error() const { return err; }

coro::task<void> buffered_reader::fill()
{
    if (buf.size() == buf.capacity()) co_return;

    auto start     = buf.size();
    auto read_more = buf.capacity() - start;
    buf.resize(buf.capacity());
    auto res = co_await impl->read(std::span{buf.data() + start, read_more});
    buf.resize(start + res.count);
    err = res.err;
}

}
