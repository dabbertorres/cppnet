#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

#include "io.hpp"
#include "reader.hpp"

namespace net
{

template<typename D>
class buffered_reader : public reader<D>
{
public:
    buffered_reader(reader<D>& impl, size_t bufsize = 1024)
        : impl{impl}
        , buf(bufsize)
    {
        buf.resize(0);
    }

    io_result read(D* data, size_t length)
    {
        // easy way out
        if (length <= buf.size())
        {
            auto end   = std::copy_n(buf.begin(), length, data);
            auto count = std::distance(buf.begin(), end);

            std::copy_backward(end, buf.end(), end);
            buf.resize(count);

            return {.count = count};
        }

        size_t total = 0;

        while (total < length)
        {
            // fill up to capacity
            auto res = ensure();
            if (res.err) return {.count = total + res.count, .err = res.err};

            // now read out of the buffer
            auto end = std::copy_n(buf.begin(), length - total, data + total);
            total += std::distance(buf.begin(), end);
        }

        return {.count = total};
    }

    // peek returns the next element if available.
    // If the buffer is currently empty, it is read to capacity (if possible).
    // If the buffer is empty, and the reader has no data available, the zero
    // value is returned.
    D peek()
    {
        // try not to read more if we don't need to
        if (!buf.empty()) return buf.front();

        auto res = ensure();
        if (!buf.empty()) return buf.front();

        return {};
    }

    // peek returns a pointer to up to length elements.
    // If length > than the buffer capacity, a null pointer is returned.
    // If less than length elements are available, length is updated to the amount
    // available.
    //
    // NOTE: peek never reads more data.
    D* peek(size_t& length)
    {
        if (length > buf.capacity()) return nullptr;

        length = buf.size();
        return buf.data();
    }

    // ensure reads more data from the underlying reader so that the size is
    // at least equal to n.
    // The amount of available data is returned in the result.
    // If n == 0, it is read to capacity.
    // If the buffer is already at capacity, or n is greater than the capacity,
    // no new data is read.
    io_result ensure(size_t n = 0)
    {
        if (n > buf.capacity()) return {.count = buf.size()};
        if (buf.size() == buf.capacity()) return {.count = buf.size()};
        if (n != 0 && buf.size() >= n) return {.count = buf.size()};

        auto start     = buf.size();
        auto read_more = n - start;
        buf.resize(read_more);
        auto res = impl.read(buf.data() + start, read_more);
        buf.resize(start + res.count);
        return res;
    }

    [[nodiscard]] size_t capacity() const noexcept { return buf.capacity(); }
    [[nodiscard]] size_t size() const noexcept { return buf.size(); }

    void reset() { buf.resize(0); }

private:
    reader<D>&     impl;
    std::vector<D> buf;
};

}
