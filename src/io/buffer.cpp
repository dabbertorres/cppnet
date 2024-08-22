#include "io/buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>

#include "coro/task.hpp"
#include "io/io.hpp"

namespace net::io
{

coro::task<result> buffer::write(std::span<const std::byte> data)
{
    if (content.size() - write_at < data.size())
    {
        content.resize(content.size() - write_at + data.size());
    }

    std::copy(data.begin(), data.end(), &content[write_at]);
    co_return {.count = data.size()};
}

coro::task<result> buffer::read(std::span<std::byte> data)
{
    auto read_amount = std::min(data.size(), content.size() - read_at);
    std::copy_n(&content[read_at], read_amount, data.begin());
    read_at += read_amount;

    result r{.count = read_amount};
    if (read_at >= content.size()) r.err = io::status_condition::closed; // TODO: should be EOF

    co_return r;
}

std::string buffer::to_string() const
{
    return std::string{reinterpret_cast<const char*>(content.data()), content.size()};
}

}
