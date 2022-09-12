#pragma once

#include <algorithm>
#include <iterator>
#include <vector>

#include "io.hpp"
#include "writer.hpp"

namespace net
{

template<typename D>
class buffered_writer : public writer<D>
{
public:
    buffered_writer(writer<D>& underlying, size_t bufsize = 1024)
        : impl{underlying}
        , buf(bufsize)
    {
        buf.resize(0);
    }

    io_result write(const D* data, size_t length)
    {
        size_t total = 0;

        while (total < length)
        {
            // fill up the buffer as much as possible
            auto available = buf.capacity() - buf.size();
            if (available > 0)
            {
                auto add   = std::min(available, length - total);
                auto start = buf.begin() + add;
                buf.resize(buf.size() + add);
                auto end = std::copy_n(data, available, start);
                total += std::distance(start, end);
            }

            // if the buffer is full, flush it
            if (buf.size() == buf.capacity()) flush_available();
        }

        return {.count = total};
    }

    io_result flush()
    {
        if (buf.empty()) return {};
        return flush_available();
    }

    [[nodiscard]] size_t capacity() const noexcept { return buf.capacity(); }
    [[nodiscard]] size_t size() const noexcept { return buf.size(); }

    void reset() { buf.resize(0); }

private:
    io_result flush_available()
    {
        auto res = impl.write(buf.data(), buf.size());
        if (res.count < buf.size())
        {
            // if short, adjust
            auto end = buf.begin() + res.count;
            std::copy_backward(end, buf.end(), end);
            buf.resize(res.count);
        }
        else
        {
            buf.resize(0);
        }

        return res;
    }

    writer<D>&     impl;
    std::vector<D> buf;
};

}
