#include "io/limit_writer.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <system_error>

#include "coro/task.hpp"
#include "io/io.hpp"

namespace net::io
{

coro::task<result> limit_writer::write(std::span<const std::byte> data)
{
    const std::size_t amount = std::min(data.size(), left);
    if (amount == 0) return coro::ready_task<result>(finish({}, amount, data.size()));

    // the writer often finishes without a coroutine (e.g. a buffered one, with room): then so can this.
    auto inner = out->write(data.first(amount));
    if (inner.finished_without_coroutine())
        return coro::ready_task<result>(finish(std::move(inner).result(), amount, data.size()));

    return finish_after(std::move(inner), amount, data.size());
}

coro::task<result> limit_writer::finish_after(coro::task<result> inner, std::size_t amount, std::size_t wanted)
{
    co_return finish(co_await std::move(inner), amount, wanted);
}

result limit_writer::finish(result res, std::size_t amount, std::size_t wanted) noexcept
{
    left -= res.count;
    if (res.err || res.count < amount) return res;

    if (amount < wanted) res.err = std::make_error_condition(std::errc::value_too_large);
    return res;
}
}
