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

    // the writer often finishes without a coroutine (e.g. a buffered one, with room): then so can this, and one that
    // waits is finishing's, rather than a coroutine's.
    return finishing.adapt(
        [this, data, amount]
        {
            waiting_amount = amount;
            waiting_wanted = data.size();
            return out->write(data.first(amount));
        },
        &busy_result);
}

result limit_writer::finish(result res, std::size_t amount, std::size_t wanted) noexcept
{
    left -= res.count;
    if (res.err || res.count < amount) return res;

    if (amount < wanted) res.err = std::make_error_code(std::errc::value_too_large);
    return res;
}
}
