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

    result res;
    if (amount > 0)
    {
        res = co_await out->write(data.first(amount));
        left -= res.count;
        if (res.err || res.count < amount) co_return res;
    }

    if (amount < data.size()) res.err = std::make_error_condition(std::errc::value_too_large);
    co_return res;
}

}
