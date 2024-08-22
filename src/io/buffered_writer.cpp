#include "io/buffered_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"

namespace net::io
{

buffered_writer::buffered_writer(writer* underlying, std::size_t bufsize)
    : impl(underlying)
    , buf(bufsize)
{
    buf.resize(0);
}

coro::task<result> buffered_writer::write(std::span<const std::byte> data)
{
    if (data.empty()) co_return result{.count = 0};

    std::size_t total = 0;

    // fill up the buffer as much as possible first...
    if (buf.size() < buf.capacity())
    {
        auto start = buf.begin();
        std::advance(start, buf.size());
        auto available = std::min(buf.capacity() - buf.size(), data.size());
        buf.resize(buf.size() + available);
        std::copy_n(data.begin(), available, start);

        total += available;
    }

    // we might be done!
    if (total == data.size()) co_return result{.count = data.size()};

    // Note that we always write to the inner reader in buf.capacity() increments.

    // At this point, the buffer is full, and we have more to write, so flush it.
    if (buf.size() == buf.capacity())
    {
        auto res = co_await flush_available();
        if (res.err) co_return res;
    }

    // At this point, the buffer is now empty. As long as writes are larger than the capacity, bypass the buffer.

    while (data.size() - total > buf.capacity())
    {
        auto res = co_await impl->write(data.subspan(total, buf.capacity()));
        if (res.err) co_return result{.count = total + res.count, .err = res.err};

        total += res.count;
    }

    // the rest of the data, if any, is less than the buffer capacity, so buffer it.
    // Note that the buffer is still empty at this point.

    auto leftover = data.size() - total;
    if (leftover > 0)
    {
        buf.resize(leftover);
        std::copy_n(data.subspan(total).begin(), leftover, buf.begin());
        total += leftover;
    }

    co_return result{.count = total};
}

int buffered_writer::native_handle() const noexcept { return impl->native_handle(); }

coro::task<result> buffered_writer::flush()
{
    std::size_t total = 0;

    while (total < buf.size())
    {
        auto res = co_await impl->write(std::span{buf}.subspan(total));
        if (res.err) co_return result{.count = total + res.count, .err = res.err};

        total += res.count;
    }

    buf.resize(0);

    co_return result{.count = total};
}

std::size_t buffered_writer::capacity() const noexcept { return buf.capacity(); }
std::size_t buffered_writer::size() const noexcept { return buf.size(); }

void buffered_writer::reset(writer* other)
{
    if (other != nullptr) impl = other;
    reset();
}

// reset clears the buffer.
void buffered_writer::reset() { buf.resize(0); }

coro::task<result> buffered_writer::flush_available()
{
    auto res = co_await impl->write(std::span{buf});
    if (res.count < buf.size())
    {
        // if short, adjust
        auto end = buf.begin();
        std::advance(end, res.count);

        auto leftover = buf.size() - res.count;
        auto new_end  = buf.begin();
        std::advance(new_end, leftover);

        std::copy_backward(end, buf.end(), new_end);
        buf.resize(leftover);
    }
    else
    {
        buf.resize(0);
    }

    co_return res;
}

}
