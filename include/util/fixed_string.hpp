#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace net::util
{

// fixed_string is a string that can be a template argument: e.g. template<fixed_string S> struct x, used as x<"abc">.
template<std::size_t N>
struct fixed_string
{
    // NOTE: public, as a template argument's type must be structural. Null-terminated: N counts the terminator.
    std::array<char, N> data{};

    constexpr fixed_string() = default;

    // NOLINTNEXTLINE(*-explicit-constructor,*-avoid-c-arrays): string literals convert, as template arguments.
    consteval fixed_string(const char (&str)[N]) { std::ranges::copy(str, data.begin()); }

    [[nodiscard]] static constexpr std::size_t size() noexcept { return N - 1; }

    [[nodiscard]] constexpr std::string_view view() const noexcept { return {data.data(), N - 1}; }

    template<std::size_t M>
    [[nodiscard]] constexpr bool operator==(const fixed_string<M>& other) const noexcept
    {
        return view() == other.view();
    }
};

template<std::size_t N>
// NOLINTNEXTLINE(*-avoid-c-arrays)
fixed_string(const char (&)[N]) -> fixed_string<N>;

template<std::size_t N, std::size_t M>
consteval fixed_string<N + M - 1> operator+(const fixed_string<N>& lhs, const fixed_string<M>& rhs)
{
    fixed_string<N + M - 1> out;
    std::ranges::copy(lhs.view(), out.data.begin());
    std::ranges::copy(rhs.view(), out.data.begin() + lhs.size());
    return out;
}

}
