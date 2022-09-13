#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <vector>

#include "util/result.hpp"

#include "io.hpp"
#include "reader.hpp"

namespace net::io
{

template<typename D>
class buffered_reader : public reader<D>
{
public:
    buffered_reader(reader<D>& impl, size_t bufsize = 1024)
        : impl{&impl}
        , buf(bufsize)
    {
        buf.resize(0);
    }

    result read(D* data, size_t length) override
    {
        // easy way out
        if (length <= buf.size())
        {
            std::copy_n(buf.begin(), length, data);
            auto end = buf.begin() + length;

            std::copy_backward(end, buf.end(), end);
            buf.resize(buf.size() - length);

            return {.count = length};
        }

        // Note that we always read from the inner reader in buf.capacity() increments.

        size_t total = 0;

        if (!buf.empty())
        {
            // copy over what we can...

            std::copy_n(buf.begin(), buf.size(), data);
            total += buf.size();
            buf.resize(0);
        }

        // At this point, the buffer is empty. As long as length - total is larger
        // than the buffer capacity, we can skip a copy and read straight to data.

        while (length - total > buf.capacity())
        {
            auto res = impl->read(data + total, buf.capacity());
            if (res.err) return {.count = total + res.count, .err = res.err};

            total += res.count;
        }

        // partial read of buffer capacity
        auto leftover = length - total;
        if (leftover > 0)
        {
            // read in a buf.capacity() increment
            buf.resize(buf.capacity());
            auto res = impl->read(buf.data(), buf.size());

            // regardless of error, give the user what we can

            buf.resize(res.count);

            auto available = std::min(leftover, res.count);
            std::copy_n(buf.begin(), available, data + total);

            total += available;

            auto end = buf.begin() + available;
            std::copy_backward(end, buf.end(), end);

            buf.resize(buf.size() - available);

            // only report an error if we couldn't fulfill the caller's request
            if (res.err && res.count < leftover) return {.count = total, .err = res.err};
        }

        return {.count = total};
    }

    // read_until returns the data up to end (consuming it, but not returning it).
    //
    // An error is returned if a read error occurs, which will have the error.
    // No more data than the buffer size will be read. If the buffer does not contain end,
    // an error a the amount of data in the buffer is returned.
    //
    // NOTE: no new data is read. Use ensure() to fill the buffer.
    util::result<std::basic_string<D>, size_t> read_until(std::basic_string_view<D> until)
    {
        const auto end = buf.end();
        for (auto iter = buf.begin(); iter != end; ++iter)
        {
            if (std::equal(until.begin(), until.end(), iter))
            {
                size_t length = std::distance(buf.begin(), iter);

                // read up to and including until, but then discard the until part
                std::basic_string<D> out(length + until.length(), 0);
                read(out.data(), out.size());

                return {.value = out.substr(0, length)};
            }
        }

        // not found
        return {.error = buf.size()};
    }

    // peek returns the next element if available.
    //
    // NOTE: peek does not implicitly read more data, instead returning the zero value if none is immediately available.
    //       See ensure if you want to make sure data is available.
    D peek()
    {
        if (!buf.empty()) return buf.front();
        return {};
    }

    // peek returns a pointer to up to length elements.
    // If length > than the buffer capacity, a null pointer is returned.
    // If less than length elements are available, length is updated to the amount
    // available.
    // If zero elements are available, a null pointer is returned.
    //
    // NOTE: peek never reads more data. See ensure if you want to make sure data is available.
    D* peek(size_t& length)
    {
        if (length > buf.capacity()) return nullptr;

        length = buf.size();
        if (length == 0) return nullptr;
        return buf.data();
    }

    // peek_next_is returns true if the next value in the buffer is equal to next, otherwise false.
    //
    // NOTE: peek_next_is does not implicit read more data, instead returning false if none is immediately available.
    //       See ensure if you want to make sure data is available.
    bool peek_next_is(D next)
    {
        if (buf.empty()) return false;

        return next == buf.front();
    }

    // peek_next_is returns true if the next view.length() values in the buffer are equal to next, otherwise false.
    //
    // NOTE: peek_next_is does not implicit read more data, instead returning false if none is immediately available.
    //       See ensure if you want to make sure data is available.
    bool peek_next_is(std::basic_string_view<D> view)
    {
        if (buf.empty()) return false;
        if (buf.size() < view.size()) return false;

        return std::equal(view.begin(), view.end(), buf.begin());
    }

    // ensure reads more data from the underlying reader so that the size is
    // at least equal to n.
    // The amount of available data is returned in the result.
    // If n == 0, it is read to capacity.
    // If the buffer is already at capacity, or n is greater than the capacity,
    // no new data is read.
    result ensure(size_t n = 0)
    {
        if (n > buf.capacity()) return {.count = buf.size()};
        if (buf.size() == buf.capacity()) return {.count = buf.size()};
        if (n != 0 && buf.size() >= n) return {.count = buf.size()};

        auto start     = buf.size();
        auto read_more = n - start;
        buf.resize(read_more);
        auto res = impl->read(buf.data() + start, read_more);
        buf.resize(start + res.count);
        return res;
    }

    [[nodiscard]] size_t capacity() const noexcept { return buf.capacity(); }
    [[nodiscard]] size_t size() const noexcept { return buf.size(); }

    // reset clears the buffer, and if other is not null, switches to it.
    // If other is null, the current reader is kept.
    // This allows re-use of the buffer's memory with other readers.
    void reset(reader<D>* other)
    {
        if (other != nullptr) impl = other;
        buf.resize(0);
    }

private:
    reader<D>*     impl;
    std::vector<D> buf;
};

}
