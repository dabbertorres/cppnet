#include "io/buffer.hpp"

#include <algorithm>
#include <span>
#include <string>

#include "coro/task.hpp"
#include "io/io.hpp"

namespace net::io
{

coro::task<result> buffer::write(std::span<const std::byte> data)
{
    content.insert(content.end(), data.begin(), data.end());
    co_return {.count = data.size()};
}

coro::task<result> buffer::read(std::span<std::byte> data)
{
    if (data.empty()) co_return {.count = 0};

    auto unread = std::span{content}.subspan(read_at);
    if (unread.empty()) co_return {.count = 0, .err = make_error_condition(status_condition::closed)};

    auto read_amount = std::min(data.size(), unread.size());
    std::copy_n(unread.begin(), read_amount, data.begin());
    read_at += read_amount;

    co_return {.count = read_amount};
}

std::string buffer::to_string() const
{
    return std::string{reinterpret_cast<const char*>(content.data()), content.size()};
}

}
