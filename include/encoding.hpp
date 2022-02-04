#pragma once

#include <concepts>

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

};
