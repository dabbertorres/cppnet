#pragma once

#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::io
{

template<typename D>
class limit_reader : public reader<D>
{
public:
    constexpr limit_reader() = default;
    constexpr limit_reader(reader<D>* reader, size_t limit)
        : parent{reader}
        , limit{limit}
    {}

    result read(D* data, size_t length) override
    {
        if (progress >= limit) return {};

        const size_t read_amount = std::min(length, limit - progress);

        auto res = parent->read(data, read_amount);
        progress += res.count;
        return res;
    }

private:
    reader<D>* parent   = nullptr;
    size_t     limit    = 0;
    size_t     progress = 0;
};

}
