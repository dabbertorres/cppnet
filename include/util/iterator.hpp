#pragma once

#include <functional>
#include <optional>
#include <ranges>

namespace net::util
{

template<std::ranges::input_range R,
         typename I    = std::ranges::borrowed_iterator_t<R>,
         typename T    = std::iter_value_t<I>,
         typename Proj = std::identity>
std::optional<T> optional_from_iterator(R&& range, I iter) noexcept
{
    if (iter != std::ranges::end(range)) return *iter;

    return std::nullopt;
}

}
