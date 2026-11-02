#include "io/limit_reader.hpp"

#include <algorithm>
#include <span>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"

namespace
{

// busy is what an operation of a reader or writer that's taken by another (waiting) makes.
net::io::result busy() noexcept { return {.err = std::make_error_code(std::errc::device_or_resource_busy)}; }

}

namespace net::io
{

coro::task<result> limit_reader::read(std::span<std::byte> data)
{
    if (progress >= limit)
        return coro::ready_task<result>(result{.count = 0, .err = make_error_code(errc::end_of_stream)});

    if (at_start && !data.empty()) [[unlikely]]
        return start_then_read(data);

    // the reader often finishes without a coroutine (e.g. a buffered one, with enough buffered): then so can this, and
    // one that waits is counting's, rather than a coroutine's.
    return counting.adapt([this, data] { return parent->read(data.first(std::min(data.size(), limit - progress))); },
                          &busy);
}

coro::task<result> limit_reader::start_then_read(std::span<std::byte> data)
{
    const auto callback = std::exchange(at_start, {});
    if (auto err = co_await callback.call(callback.context)) co_return result{.err = err};
    co_return co_await read(data);
}

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
