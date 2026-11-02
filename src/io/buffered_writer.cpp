#include "io/buffered_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"

namespace
{

// busy is what an operation of a reader or writer that's taken by another (waiting) makes.
net::io::result busy() noexcept { return {.err = std::make_error_code(std::errc::device_or_resource_busy)}; }

}

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
    // it fills the buffer, flushes it, and writes the rest: without a coroutine, unless a write has to wait, which
    // writing finishes (see wrote()).
    return writing.adapt(
        [this, data] -> coro::step<result, result>
        {
            // fill up the buffer as much as possible first...
            const auto available = std::min(buf.capacity() - buf.size(), data.size());
            buf.insert(buf.end(), data.begin(), std::next(data.begin(), static_cast<std::ptrdiff_t>(available)));

            // ... then, as there's more to write, all of it must be flushed before writing anything else, or the output
            // would be out of order.
            what            = pending::through_flush;
            written         = 0;
            through         = data;
            through_written = available;
            return impl->write(std::span<const std::byte>{buf});
        },
        &busy);
}

coro::step<result, result> buffered_writer::write_rest()
{
    // the buffer's empty: as long as what's left is larger than the capacity, bypass the buffer: all of it at once, so
    // a large write (e.g. a WebSocket's message, or a body) takes as few writes of what's wrapped as it can (each a
    // syscall, or a coroutine).
    if (through.size() - through_written > buf.capacity())
    {
        what = pending::through;
        return impl->write(through.subspan(through_written));
    }

    // the rest, if any, is no more than the buffer's capacity, and the buffer is empty, so buffer it.
    buf.insert(buf.end(), through.subspan(through_written).begin(), through.end());
    return result{.count = through.size()};
}

coro::step<result, result> buffered_writer::wrote(result res)
{
    if (what == pending::through)
    {
        through_written += res.count;
        if (res.err) return result{.count = through_written, .err = res.err};
        if (res.count == 0) return result{.count = through_written, .err = make_error_code(errc::no_progress)};
        return write_rest();
    }

    written += res.count;
    if (!res.err && res.count != 0 && written < buf.size()) return impl->write(std::span{buf}.subspan(written));

    auto flush_result = flushed(written, res);
    if (what == pending::flush) return flush_result;

    // a write that doesn't fit in the buffer: what was buffered has gone, or it failed.
    if (flush_result.err) return result{.count = through_written, .err = flush_result.err};
    return write_rest();
}

handle buffered_writer::native_handle() const noexcept { return impl->native_handle(); }

std::span<std::byte> buffered_writer::prepare(std::size_t size)
{
    if (size > buf.capacity() - buf.size()) return {};

    const auto start = buf.size();
    buf.resize(start + size);
    return std::span{buf}.subspan(start);
}

coro::task<result> buffered_writer::flush()
{
    if (buf.empty()) return coro::ready_task<result>();
    if (writing.is_claimed()) return coro::ready_task<result>(busy());

    // usually, the writes don't wait (e.g. a socket with room for it all): then it needs no coroutine, nor writing,
    // which finishes one that has to wait (see flush_waiting()).
    std::size_t total = 0;
    while (true)
    {
        auto inner = impl->write(std::span<const std::byte>{buf}.subspan(total));
        if (!inner.finished_without_coroutine()) return flush_waiting(std::move(inner), total);

        const auto res = std::move(inner).result();
        total += res.count;
        if (res.err || res.count == 0 || total == buf.size()) return coro::ready_task<result>(flushed(total, res));
    }
}

coro::task<result> buffered_writer::flush_waiting(coro::task<result> inner, std::size_t total)
{
    return writing.adapt(
        [this, total, &inner]
        {
            what    = pending::flush;
            written = total;
            return std::move(inner);
        },
        &busy);
}

result buffered_writer::flushed(std::size_t total, const result& last) noexcept
{
    // drop what was written, keeping anything that wasn't for a later flush.
    buf.erase(buf.begin(), std::next(buf.begin(), static_cast<std::ptrdiff_t>(total)));

    // a write that made no progress without reporting why would otherwise leave the rest unwritten, unreported.
    if (!last.err && last.count == 0 && !buf.empty())
        return result{.count = total, .err = make_error_code(errc::no_progress)};

    return result{.count = total, .err = last.err};
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
