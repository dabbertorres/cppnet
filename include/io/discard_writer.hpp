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

    coro::task<result> write(std::span<const std::byte> data) override { co_return {.count = data.size()}; }

    [[nodiscard]] int native_handle() const noexcept override { return -1; }
};

}
