#include "io/flush_before_read.hpp"

#include <cstddef>
#include <span>
#include <utility>

#include "coro/task.hpp"
#include "io/io.hpp"

namespace net::io
{

coro::task<result> flush_before_read::read(std::span<std::byte> data)
{
    if (writer->size() == 0) return conn->read(data);

    // usually, the flush doesn't wait: then there's no need for a coroutine of its own (see coro::ready_task).
    auto flushing = writer->flush();
    if (!flushing.finished_without_coroutine()) return flush_and_read(std::move(flushing), data);

    if (std::move(flushing).result().err) return coro::ready_task<result>(closed());
    return conn->read(data);
}

coro::task<result> flush_before_read::flush_and_read(coro::task<result> flushing, std::span<std::byte> data)
{
    if ((co_await std::move(flushing)).err) co_return closed();
    co_return co_await conn->read(data);
}

result flush_before_read::closed() noexcept
{
    // the other end can't be answered any more, which ends the connection as its closing would.
    return result{.err = make_error_code(errc::end_of_stream)};
}
}
