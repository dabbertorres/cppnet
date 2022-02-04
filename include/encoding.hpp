#pragma once

#include <concepts>

#include "exception.hpp"
#include "reader.hpp"
#include "writer.hpp"

namespace net
{

template<typename T>
concept Encoder = requires(const T& t, writer& w)
{
    // clang-format off
    { t.encode(w) } -> std::same_as<void>;
    // clang-format on
};

template<typename T>
concept Decoder = requires(T& t, reader& r)
{
    // clang-format off
    { t.decode(r) } -> std::same_as<void>;
    // clang-format on
};

template<typename T>
reader& operator>>(reader& r, T& t)
{
    auto* data = reinterpret_cast<uint8_t*>(&t);
    if (auto read = r.read(data, sizeof(T)); read < sizeof(T)) throw exception{};
    return r;
}

template<Decoder T>
reader& operator>>(reader& r, T& t)
{
    t.decode(r);
    return r;
}

template<typename T>
writer& operator<<(writer& w, const T& t)
{
    const auto* data = reinterpret_cast<const uint8_t*>(&t);
    if (auto wrote = w.write(data, sizeof(T)); wrote < sizeof(T)) throw exception{};
    return w;
}

template<Encoder T>
writer& operator>>(writer& w, const T& t)
{
    t.encode(w);
    return w;
}

};
