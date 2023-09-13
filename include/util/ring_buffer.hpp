#pragma once

#include <array>
#include <cstddef>

namespace net::util
{

template<typename T, std::size_t N>
class ring_buffer
{
public:
    [[nodiscard]] constexpr bool        empty() noexcept { return current_size == 0; }
    [[nodiscard]] constexpr std::size_t capacity() noexcept { return N; }
    [[nodiscard]] constexpr std::size_t size() noexcept { return current_size; }

    void push(T&& value) noexcept
    {
        auto idx    = (begin + current_size) % capacity();
        values[idx] = value;
        if (current_size + 1 > capacity()) begin = (begin + 1) % capacity(); // drop oldest
        else ++current_size;
    }

    T pop() noexcept
    {
        auto value = values[begin];
        begin      = (begin + 1) % capacity();
        if (current_size != 0) --current_size;
        return value;
    }

    T& operator[](std::size_t index) noexcept { return values[(begin + index) % capacity()]; }

    const T& operator[](std::size_t index) const noexcept { return values[(begin + index) % capacity()]; }

private:
    std::array<T, N> values{};
    std::size_t      begin        = 0;
    std::size_t      current_size = 0;
};

}
