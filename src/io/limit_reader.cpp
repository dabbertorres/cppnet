#include "io/limit_reader.hpp"

#include <algorithm>
#include <span>
#include <system_error>

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
    if (inner.finished_without_coroutine()) return coro::ready_task<result>(counted(std::move(inner).result()));

    return count_after(std::move(inner));
}

coro::task<result> limit_reader::count_after(coro::task<result> inner) { co_return counted(co_await std::move(inner)); }

result limit_reader::counted(result res) noexcept
{
    const bool before = progress < limit;
    progress += res.count;

    if (before && progress >= limit) at_end();

    // NOTE: exactly the end of the stream: any other failure is reported as it is.
    if (how == mode::exact && progress < limit && res.err == errc::end_of_stream)
        res.err = std::make_error_code(std::errc::connection_aborted);

    return res;
}

}
