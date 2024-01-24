#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <vector>

#include "io.hpp"
#include "writer.hpp"

namespace net::io
{

class buffered_writer : public writer
{
public:
    buffered_writer(writer* underlying, std::size_t bufsize = 1'024)
        : impl(underlying)
        , buf(bufsize)
    {
        buf.resize(0);
    }

    result write(std::span<const std::byte> data) override
    {
        if (data.empty()) return {.count = 0};

        std::size_t total = 0;

        // fill up the buffer as much as possible first...
        if (buf.size() < buf.capacity())
        {
            auto start = buf.begin();
            std::advance(start, buf.size());
            auto available = std::min(buf.capacity() - buf.size(), data.size());
            buf.resize(buf.size() + available);
            std::copy_n(data.begin(), available, start);

            total += available;
        }

        // we might be done!
        if (total == data.size()) return {.count = data.size()};

        // Note that we always write to the inner reader in buf.capacity() increments.

        // At this point, the buffer is full, and we have more to write, so flush it.
        if (buf.size() == buf.capacity())
        {
            auto res = flush_available();
            if (res.err) return res;
        }

        // At this point, the buffer is now empty. As long as writes are larger than the capacity, bypass the buffer.

        while (data.size() - total > buf.capacity())
        {
            auto res = impl->write(data.subspan(total, buf.capacity()));
            if (res.err) return {.count = total + res.count, .err = res.err};

            total += res.count;
        }

        // the rest of the data, if any, is less than the buffer capacity, so buffer it.
        // Note that the buffer is still empty at this point.

        auto leftover = data.size() - total;
        if (leftover > 0)
        {
            buf.resize(leftover);
            std::copy_n(data.subspan(total).begin(), leftover, buf.begin());
            total += leftover;
        }

        return {.count = total};
    }

    using writer::write;

    [[nodiscard]] int native_handle() const noexcept override { return impl->native_handle(); }

    result flush()
    {
        if (buf.empty()) return {};
        return flush_available();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return buf.capacity(); }
    [[nodiscard]] std::size_t size() const noexcept { return buf.size(); }

    // reset clears the buffer, and if other is not null, switches to it.
    // If other is null, the current writer is kept.
    // This allows re-use of the buffer's memory with other writers.
    void reset(writer* other)
    {
        if (other != nullptr) impl = other;
        reset();
    }

    // reset clears the buffer.
    void reset() { buf.resize(0); }

private:
    result flush_available()
    {
        auto res = impl->write(std::span{buf});
        if (res.count < buf.size())
        {
            // if short, adjust
            auto end = buf.begin();
            std::advance(end, res.count);

            auto leftover = buf.size() - res.count;
            auto new_end  = buf.begin();
            std::advance(new_end, leftover);

            std::copy_backward(end, buf.end(), new_end);
            buf.resize(leftover);
        }
        else
        {
            buf.resize(0);
        }

        return res;
    }

    writer*                impl;
    std::vector<std::byte> buf;
};

}
