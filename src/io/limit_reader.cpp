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
    {
        co_return {
            .count = 0,
            .err   = make_error_condition(status_condition::closed),
        };
    }

    const std::size_t read_amount = std::min(data.size(), limit - progress);

    auto res = co_await parent->read(data.subspan(0, read_amount));
    progress += res.count;
    co_return res;
}

}
