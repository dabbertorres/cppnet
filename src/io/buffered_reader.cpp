#include "io/buffered_reader.hpp"

namespace net::io
{

buffered_reader::buffered_reader(reader& impl, size_t bufsize)
    : impl{&impl}
    , buf(bufsize)
{
    buf.resize(0);
}

result buffered_reader::read(byte* data, size_t length)
{
    if (length == 0) return {.count = 0};

    // easy way out
    if (length <= buf.size())
    {
        std::copy_n(buf.begin(), length, data);

        auto end = buf.begin();
        std::advance(end, length);

        auto leftover = buf.size() - length;
        auto new_end  = buf.begin();
        std::advance(new_end, leftover);

        std::copy_backward(end, buf.end(), new_end);
        buf.resize(leftover);

        return {.count = length};
    }

    // Note that we always (try to) read from the inner reader in buf.capacity() increments.

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
        fill();

        // regardless of error, give the user what we can

        auto available = std::min(leftover, buf.size());
        std::copy_n(buf.begin(), available, data + total);

        total += available;

        auto end = buf.begin();
        std::advance(end, available);
        std::copy_backward(end, buf.end(), end);

        buf.resize(buf.size() - available);

        // only report an error if we couldn't fulfill the caller's request
        if (available < leftover) return {.count = total, .err = err};
    }

    return {.count = total};
}

[[nodiscard]] std::tuple<byte, bool> buffered_reader::peek()
{
    if (!buf.empty()) return {buf.front(), true};
    fill();

    if (buf.empty()) return {static_cast<byte>(0), false};
    return {buf.front(), true};
}

void buffered_reader::reset(reader* other)
{
    if (other != nullptr) impl = other;
    buf.resize(0);
    err.clear();
}

void buffered_reader::reset() { buf.resize(0); }

std::error_condition buffered_reader::error() const { return err; }

void buffered_reader::fill()
{
    if (buf.size() == buf.capacity()) return;

    auto start     = buf.size();
    auto read_more = buf.capacity() - start;
    buf.resize(buf.capacity());
    auto res = impl->read(buf.data() + start, read_more);
    buf.resize(start + res.count);
    err = res.err;
}

}
