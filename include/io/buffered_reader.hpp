#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <type_traits>
#include <vector>

#include "util/result.hpp"

#include "io.hpp"
#include "reader.hpp"

namespace net::io
{

template<typename T>
concept ReadUntilContainer = requires(T& t) {
                                 std::is_constructible_v<T, size_t>;
                                 t.data();
                                 t.size();
                                 t.resize(0);
                             };

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
        if (length == 0) return {.count = 0};

        // easy way out
        if (length <= buf.size())
        {
            std::copy_n(buf.begin(), length, data);

            auto end      = buf.begin() + length;
            auto leftover = buf.size() - length;

            std::copy_backward(end, buf.end(), buf.begin() + leftover);
            buf.resize(leftover);

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

    // read_until returns the data up to until (consuming it, but not returning it).
    //
    // An error is returned if a read error occurs, which will have the error.
    // No more data than the buffer size will be read. If the buffer does not contain until,
    // the amount of data in the buffer is returned.
    //
    // NOTE: no new data is read. Use ensure() to fill the buffer.
    template<ReadUntilContainer C>
    util::result<C, size_t> read_until(std::basic_string_view<D> until,
                                       size_t                    max_look = std::numeric_limits<size_t>::max())
    {
        std::optional<size_t> maybe = find(until, max_look);
        if (!maybe.has_value()) return {buf.size()};

        C out(maybe.value(), D{});
        read(out.data(), out.size() + until.size()); // won't fail, as everything is in the buffer
        out.resize(maybe.value());
        return {out};
    }

    // find returns the amount of data between the current position and until (inclusive).
    // If it is not found, an empty optional is returned.
    // max_look may be specified to limit how far the search will go.
    std::optional<size_t> find(std::basic_string_view<D> until, size_t max_look = std::numeric_limits<size_t>::max())
    {
        if (until.size() > buf.size()) return std::nullopt;

        auto end = buf.end();
        if (max_look != std::numeric_limits<size_t>::max())
        {
            end = buf.begin();
            std::advance(end, max_look - until.length());
        }
        else
        {
            // only look forward as long as there are enough characters left in the buffer.
            std::advance(end, -until.length() - 1);
        }

        for (auto iter = buf.begin(); iter != end; ++iter)
        {
            if (std::equal(until.begin(), until.end(), iter))
            {
                size_t length = std::distance(buf.begin(), iter);
                return {length};
            }
        }

        return std::nullopt;
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

        if (n == 0) n = buf.capacity();

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

    // reset clears the buffer.
    void reset() { buf.resize(0); }

private:
    reader<D>*     impl;
    std::vector<D> buf;
};

}
