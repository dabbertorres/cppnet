#pragma once

#include <concepts>

#include "reader.hpp"
#include "writer.hpp"

namespace net
{

template<typename T, typename D>
concept Encoder = requires(const T& t, writer<D>& w)
{
    // clang-format off
    { t.encode(w) } -> std::same_as<void>;
    // clang-format on
};

template<typename T, typename D>
concept Decoder = requires(T& t, reader<D>& r)
{
    // clang-format off
    { t.decode(r) } -> std::same_as<void>;
    // clang-format on
};

};
