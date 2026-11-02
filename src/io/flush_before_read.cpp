#include "io/flush_before_read.hpp"

#include <cstddef>
#include <span>
#include <utility>

#include "coro/task.hpp"
#include "io/io.hpp"

namespace
{

// busy is what an operation of a reader or writer that's taken by another (waiting) makes.
net::io::result busy() noexcept { return {.err = std::make_error_code(std::errc::device_or_resource_busy)}; }

}

namespace net::io
{

coro::task<result> flush_before_read::read(std::span<std::byte> data)
{
    if (held || writer->size() == 0) return conn->read(data);

    // usually, the flush doesn't wait: then the read's what's wrapped's, as is. One that waits is flushing_first's,
    // which reads once it's done, rather than a coroutine's.
    auto flushing = writer->flush();
    if (!flushing.finished_without_coroutine())
    {
        return flushing_first.adapt(
            [this, data, &flushing]
            {
                target  = data;
                flushed = false;
                return std::move(flushing);
            },
            &busy);
    }

    if (std::move(flushing).result().err) return coro::ready_task<result>(closed());
    return conn->read(data);
}

coro::step<result, result> flush_before_read::read_after(result res)
{
    if (flushed) return res;
    if (res.err) return closed();

    flushed = true;
    return conn->read(target);
}

result flush_before_read::closed() noexcept
{
    // the other end can't be answered any more, which ends the connection as its closing would.
    return result{.err = make_error_code(errc::end_of_stream)};
}
}
