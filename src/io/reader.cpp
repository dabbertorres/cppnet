#include "io/reader.hpp"

#include "coro/task.hpp"
#include "io/io.hpp"

namespace net::io
{

coro::task<result> read(const scheduler& scheduler, std::byte* data, std::size_t length)
{
    // TODO
}

}
