#include "io/buffered_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
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

result buffered_reader::read(std::span<std::byte> data)
{
    if (data.empty()) return {.count = 0};

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

        return {.count = data.size()};
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
        auto res = impl->read(data.subspan(total, buf.capacity()));
        if (res.err) return {.count = total + res.count, .err = res.err};

        total += res.count;
    }

    // partial read of buffer capacity
    auto leftover = data.size() - total;
    if (leftover > 0)
    {
        fill();

        // regardless of error, give the user what we can

        auto available = std::min(leftover, buf.size());
        std::copy_n(buf.begin(), available, data.subspan(total).begin());

        total += available;

        auto end = buf.begin();
        std::advance(end, available);
        std::copy_backward(end, buf.end(), end);

        buf.resize(buf.size() - available);

        // only report an error if we couldn't fulfill the caller's request
        if (available < leftover) return {.count = total, .err = err};
    }

    return {.count = total};
}

coro::task<result> buffered_reader::co_read(std::span<std::byte> data) { co_return read(data); }

buffered_reader::read_until_result buffered_reader::read_until(std::span<const std::byte> delim) noexcept
{
    // do we already have an instance of delim in the buffer?
    auto searcher    = std::boyer_moore_searcher{delim.begin(), delim.end()};
    auto delim_begin = std::search(buf.begin(), buf.end(), searcher);
    if (delim_begin != buf.end())
    {
        return {
            .data      = {buf.begin(), delim_begin},
            .is_prefix = false,
            .err       = err,
        };
    }

    std::terminate();
}

[[nodiscard]] std::tuple<std::byte, bool> buffered_reader::peek()
{
    if (!buf.empty()) return {buf.front(), true};
    fill();

    if (buf.empty()) return {static_cast<std::byte>(0), false};
    return {buf.front(), true};
}

void buffered_reader::reset(reader* other)
{
    if (other != nullptr) impl = other;
    buf.resize(0);
    err.clear();
}

void buffered_reader::reset() { buf.resize(0); }

std::error_condition buffered_reader::error() const { return err; }

void buffered_reader::fill()
{
    if (buf.size() == buf.capacity()) return;

    auto start     = buf.size();
    auto read_more = buf.capacity() - start;
    buf.resize(buf.capacity());
    auto res = impl->read(std::span{buf.data() + start, read_more});
    buf.resize(start + res.count);
    err = res.err;
}

}
