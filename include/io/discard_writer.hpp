#pragma once

#include <cstddef>
#include <span>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"

namespace net::io
{

// discard_writer accepts everything written to it, and drops it: e.g. the body of a response to a HEAD request.
class discard_writer final : public writer
{
public:
    using writer::write;

    // NOTE: without a coroutine (see coro::ready_task), as it never waits.
    coro::task<result> write(std::span<const std::byte> data) override
    {
        return coro::ready_task<result>(result{.count = data.size()});
    }

    [[nodiscard]] handle native_handle() const noexcept override { return -1; }
};

}
