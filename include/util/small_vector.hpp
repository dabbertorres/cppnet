#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace net::util
{

// small_vector is a sequence that holds up to N elements in itself, without allocating, and moves them to a
// std::vector if it grows past that.
template<typename T, std::size_t N>
    requires std::default_initializable<T> && std::movable<T>
class small_vector
{
public:
    using value_type      = T;
    using size_type       = std::size_t;
    using reference       = T&;
    using const_reference = const T&;
    using iterator        = T*;
    using const_iterator  = const T*;

    constexpr small_vector() = default;

    template<typename... Args>
    constexpr T& emplace_back(Args&&... args)
    {
        if (!spilled())
        {
            if (count < N)
            {
                auto& slot = held[count++];
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay): args may be string literals.
                slot = T(std::forward<Args>(args)...);
                return slot;
            }

            // NOTE: made before the elements are moved, in case args refer to one.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay): args may be string literals.
            T value(std::forward<Args>(args)...);

            spill.reserve(N * 2);
            std::ranges::move(held.begin(),
                              held.begin() + static_cast<std::ptrdiff_t>(count),
                              std::back_inserter(spill));
            count = 0;

            return spill.emplace_back(std::move(value));
        }

        return spill.emplace_back(std::forward<Args>(args)...);
    }

    constexpr void push_back(T value) { emplace_back(std::move(value)); }

    // erase removes the element at pos, moving those after it back one, and returns where the next one now is.
    constexpr iterator erase(const_iterator pos)
    {
        const auto index = static_cast<std::ptrdiff_t>(pos - begin());
        if (spilled()) return spill.erase(spill.begin() + index) - spill.begin() + data();

        std::ranges::move(held.begin() + index + 1,
                          held.begin() + static_cast<std::ptrdiff_t>(count),
                          held.begin() + index);
        held[--count] = T{};
        return begin() + index;
    }

    // clear removes the elements, keeping any memory it allocated (and so, using it).
    constexpr void clear() noexcept
    {
        std::ranges::fill(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(count), T{});
        count = 0;
        spill.clear();
    }

    [[nodiscard]] constexpr size_type size() const noexcept { return spilled() ? spill.size() : count; }
    [[nodiscard]] constexpr bool      empty() const noexcept { return size() == 0; }

    // spilled is whether the elements have moved to the heap.
    [[nodiscard]] constexpr bool spilled() const noexcept { return spill.capacity() != 0; }

    [[nodiscard]] constexpr T*       data() noexcept { return spilled() ? spill.data() : held.data(); }
    [[nodiscard]] const constexpr T* data() const noexcept { return spilled() ? spill.data() : held.data(); }

    [[nodiscard]] constexpr iterator       begin() noexcept { return data(); }
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return data(); }
    [[nodiscard]] constexpr iterator       end() noexcept { return data() + size(); }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return data() + size(); }

    [[nodiscard]] constexpr T&       operator[](size_type i) noexcept { return data()[i]; }
    [[nodiscard]] const constexpr T& operator[](size_type i) const noexcept { return data()[i]; }

    [[nodiscard]] constexpr T&       front() noexcept { return *data(); }
    [[nodiscard]] const constexpr T& front() const noexcept { return *data(); }
    [[nodiscard]] constexpr T&       back() noexcept { return data()[size() - 1]; }
    [[nodiscard]] const constexpr T& back() const noexcept { return data()[size() - 1]; }

    [[nodiscard]] friend constexpr bool operator==(const small_vector& lhs, const small_vector& rhs)
    {
        return std::ranges::equal(lhs, rhs);
    }

private:
    std::array<T, N> held{};
    size_type        count = 0;
    std::vector<T>   spill;
};

}
