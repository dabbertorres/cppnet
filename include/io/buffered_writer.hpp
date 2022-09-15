#pragma once

#include <algorithm>
#include <iterator>
#include <vector>

#include "io.hpp"
#include "writer.hpp"

namespace net::io
{

template<typename D>
class buffered_writer : public writer<D>
{
public:
    buffered_writer(writer<D>& underlying, size_t bufsize = 1024)
        : impl{&underlying}
        , buf(bufsize)
    {
        buf.resize(0);
    }

    result write(const D* data, size_t length) override
    {
        if (length == 0) return {.count = 0};

        size_t total = 0;

        // fill up the buffer as much as possible first...
        if (buf.size() < buf.capacity())
        {
            auto start     = buf.begin() + buf.size();
            auto available = std::min(buf.capacity() - buf.size(), length);
            buf.resize(buf.size() + available);
            std::copy_n(data, available, start);

            total += available;
        }

        // we might be done!
        if (total == length) return {.count = length};

        // Note that we always write to the inner reader in buf.capacity() increments.

        // At this point, the buffer is full, and we have more to write, so flush it.
        if (buf.size() == buf.capacity())
        {
            auto res = flush_available();
            if (res.err) return res;
        }

        // At this point, the buffer is now empty. As long as writes are larger than the capacity, bypass the buffer.

        while (length - total > buf.capacity())
        {
            auto res = impl->write(data + total, buf.capacity());
            if (res.err) return {.count = total + res.count, .err = res.err};

            total += res.count;
        }

        // the rest of the data, if any, is less than the buffer capacity, so buffer it.
        // Note that the buffer is still empty at this point.

        auto leftover = length - total;
        if (leftover > 0)
        {
            buf.resize(leftover);
            std::copy_n(data + total, leftover, buf.begin());
            total += leftover;
        }

        return {.count = total};
    }

    result flush()
    {
        if (buf.empty()) return {};
        return flush_available();
    }

    [[nodiscard]] size_t capacity() const noexcept { return buf.capacity(); }
    [[nodiscard]] size_t size() const noexcept { return buf.size(); }

    // reset clears the buffer, and if other is not null, switches to it.
    // If other is null, the current writer is kept.
    // This allows re-use of the buffer's memory with other writers.
    void reset(writer<D>* other)
    {
        if (other != nullptr) impl = other;
        reset();
    }

    // reset clears the buffer.
    void reset() { buf.resize(0); }

private:
    result flush_available()
    {
        auto res = impl->write(buf.data(), buf.size());
        if (res.count < buf.size())
        {
            // if short, adjust
            auto end      = buf.begin() + res.count;
            auto leftover = buf.size() - res.count;

            std::copy_backward(end, buf.end(), buf.begin() + leftover);
            buf.resize(leftover);
        }
        else
        {
            buf.resize(0);
        }

        return res;
    }

    writer<D>*     impl;
    std::vector<D> buf;
};

}
