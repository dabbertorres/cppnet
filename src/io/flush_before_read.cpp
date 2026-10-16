#include "io/flush_before_read.hpp"

#include <cstddef>
#include <span>

#include "coro/task.hpp"
#include "io/io.hpp"

namespace net::io
{

coro::task<result> flush_before_read::read(std::span<std::byte> data)
{
    if (writer->size() == 0) return conn->read(data);
    return flush_and_read(data);
}

coro::task<result> flush_before_read::flush_and_read(std::span<std::byte> data)
{
    if (auto flushed = co_await writer->flush(); flushed.err)
        co_return result{.err = make_error_condition(status_condition::closed)};

    co_return co_await conn->read(data);
}

}
