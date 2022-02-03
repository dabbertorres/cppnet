#pragma once

#if defined(__clang__)
#    define NET_PACKED __attribute__((packed))
#elif defined(__GNUC__) || defined(__GNUG__)
#    define NET_PACKED __attribute__((packed))
#elif defined(_MSC_VER)
#    error "c'mon visual C++, get with the program"
#else
#    error "could not determine compiler type"
#endif

#include <concepts>
#include <type_traits>

namespace net::util
{

template<typename Result, typename Error>
class result
{
public:
    using result_type = Result;
    using error_type  = Error;

    constexpr result() = delete;

    constexpr result(Result value) noexcept : value{value}, is_error{false} {}
    constexpr result(Error error) noexcept : error{error}, is_error{true} {}

    constexpr result(const result& other) noexcept(
        std::is_nothrow_copy_constructible_v<Result>&& std::is_nothrow_copy_constructible_v<Error>)
    {
        is_error = other.is_error;
        if (other.is_error)
            error = other.error;
        else
            value = other.value;
    }

    constexpr result(result&& other) noexcept
    {
        is_error = other.is_error;
        if (other.is_error)
            error = std::move(other.error);
        else
            value = std::move(other.value);
    }

    constexpr result& operator=(const result& other) noexcept(
        std::is_nothrow_copy_constructible_v<Result>&& std::is_nothrow_copy_constructible_v<Error>)
    {
        is_error = other.is_error;
        if (other.is_error)
            error = other.error;
        else
            value = other.value;
    }

    constexpr result& operator=(result&& other) noexcept
    {
        is_error = other.is_error;
        if (other.is_error)
            error = std::move(other.error);
        else
            value = std::move(other.value);
    }

    constexpr ~result() noexcept(
        std::is_nothrow_destructible_v<Result>&& std::is_nothrow_destructible_v<Error>)
    {
        if (is_error)
            error.~Error();
        else
            value.~Result();
    }

    constexpr      operator bool() const noexcept { return !is_error; }
    constexpr bool has_value() const noexcept { return !is_error; }

    explicit constexpr operator Result() const noexcept { return value; }
    constexpr Result   to_value() const noexcept { return value; }

    explicit constexpr operator Error() const noexcept { return error; }
    constexpr Error    to_error() const noexcept { return error; }

    template<typename F>
    requires std::invocable<F, Result>
    constexpr auto if_value(F&& f) const noexcept
    {
        if (!is_error)
        {
            if constexpr (std::is_void_v<std::invoke_result_t<F, Result>>)
            {
                f(value);
                return *this;
            }
            else
                return result{f(value)};
        }

        return *this;
    }

    template<typename F>
    requires std::invocable<F, Error>
    constexpr auto if_error(F&& f) const noexcept
    {
        if (is_error)
        {
            if constexpr (std::is_void_v<std::invoke_result_t<F, Error>>)
            {
                f(error);
                return *this;
            }
            else
                return result{f(error)};
        }

        return *this;
    }

    template<std::invocable F>
    requires std::is_same_v<std::invoke_result_t<F>, Result>
    constexpr auto or_else(F&& f) const noexcept
    {
        if (is_error)
            return f();
        else
            return value;
    }

    constexpr auto or_else(Result other) const noexcept
    {
        if (is_error)
            return other;
        else
            return value;
    }

private:
    union
    {
        Result value;
        Error  error;
    };
    bool is_error;
};

}
