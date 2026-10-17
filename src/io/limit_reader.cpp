#include "io/limit_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <span>

#include "coro/task.hpp"
#include "io/io.hpp"

namespace net::io
{

coro::task<result> limit_reader::read(std::span<std::byte> data)
{
    if (progress >= limit)
        return coro::ready_task<result>(result{.count = 0, .err = make_error_code(errc::end_of_stream)});

    // the reader often finishes without a coroutine (e.g. a buffered one, with enough buffered): then so can this.
    auto inner = parent->read(data.first(std::min(data.size(), limit - progress)));
    if (inner.finished_without_coroutine())
    {
        auto res = std::move(inner).result();
        progress += res.count;
        return coro::ready_task<result>(res);
    }

    return count_after(std::move(inner));
}

coro::task<result> limit_reader::count_after(coro::task<result> inner)
{
    auto res = co_await std::move(inner);
    progress += res.count;
    co_return res;
}
}
