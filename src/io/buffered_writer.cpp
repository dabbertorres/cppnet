#include "io/buffered_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <system_error>

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
    // usually, it fits in the buffer: then there's no need for a coroutine (see coro::ready_task).
    if (data.size() <= buf.capacity() - buf.size())
    {
        buf.insert(buf.end(), data.begin(), data.end());
        return coro::ready_task<result>(result{.count = data.size()});
    }

    return write_through(data);
}

coro::task<result> buffered_writer::write_through(std::span<const std::byte> data)
{
    // fill up the buffer as much as possible first...
    auto available = std::min(buf.capacity() - buf.size(), data.size());
    buf.insert(buf.end(), data.begin(), std::next(data.begin(), static_cast<std::ptrdiff_t>(available)));

    std::size_t total = available;

    // we might be done!
    if (total == data.size()) co_return result{.count = total};

    // Note that we always write to the inner writer in buf.capacity() increments.

    // At this point, the buffer is full, and we have more to write. All of it must be flushed before writing anything
    // else, or the output would be out of order.
    auto flushed = co_await flush();
    if (flushed.err) co_return result{.count = total, .err = flushed.err};

    // At this point, the buffer is now empty. As long as writes are larger than the capacity, bypass the buffer.

    while (data.size() - total > buf.capacity())
    {
        auto res = co_await impl->write(data.subspan(total, buf.capacity()));
        total += res.count;

        if (res.err) co_return result{.count = total, .err = res.err};
        if (res.count == 0) co_return result{.count = total, .err = make_error_condition(status_condition::error)};
    }

    // the rest of the data, if any, is no more than the buffer capacity, and the buffer is empty, so buffer it.
    buf.insert(buf.end(), data.subspan(total).begin(), data.end());

    co_return result{.count = data.size()};
}

int buffered_writer::native_handle() const noexcept { return impl->native_handle(); }

std::span<std::byte> buffered_writer::prepare(std::size_t size)
{
    if (size > buf.capacity() - buf.size()) return {};

    const auto start = buf.size();
    buf.resize(start + size);
    return std::span{buf}.subspan(start);
}

coro::task<result> buffered_writer::flush()
{
    std::size_t total = 0;
    result      res;

    while (total < buf.size())
    {
        res = co_await impl->write(std::span{buf}.subspan(total));
        total += res.count;

        if (res.err) break;
        if (res.count == 0)
        {
            res.err = make_error_condition(status_condition::error);
            break;
        }
    }

    // drop what was written, keeping anything that wasn't for a later flush.
    buf.erase(buf.begin(), std::next(buf.begin(), static_cast<std::ptrdiff_t>(total)));

    co_return result{.count = total, .err = res.err};
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

}
