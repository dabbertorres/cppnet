#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <variant>

namespace net::util
{

template<typename Value, typename Error>
    requires(!std::is_same_v<Value, Error>)
class result
{
public:
    using result_type = Value;
    using error_type  = Error;

    constexpr result() = delete;

    constexpr result(const Value& value) noexcept(std::is_nothrow_copy_constructible_v<Value>)
        : value{value}
    {}

    constexpr result(Value&& value) noexcept(std::is_nothrow_move_constructible_v<Value>)
        : value{std::forward<Value>(value)}
    {}

    constexpr result(const Error& error) noexcept(std::is_nothrow_copy_constructible_v<Error>)
        : value{error}
    {}

    constexpr result(Error&& error) noexcept(std::is_nothrow_move_constructible_v<Error>)
        : value{std::forward<Error>(error)}
    {}

    constexpr result(const result& other) noexcept(
        std::is_nothrow_copy_constructible_v<Value>&& std::is_nothrow_copy_constructible_v<Error>)
        : value{other.value}
    {}

    constexpr result(result&& other) noexcept(
        std::is_nothrow_move_constructible_v<Value>&& std::is_nothrow_move_constructible_v<Error>)
        : value{std::move(other.value)}
    {}

    constexpr result& operator=(const result& other) noexcept(
        std::is_nothrow_copy_assignable_v<Value>&& std::is_nothrow_copy_assignable_v<Error>)
    {
        if (this != &other) value = other.value;
        return *this;
    }

    constexpr result& operator=(result&& other) noexcept(
        std::is_nothrow_move_assignable_v<Value>&& std::is_nothrow_move_assignable_v<Error>)
    {
        value = std::move(other.value);
        return *this;
    }

    constexpr ~result() noexcept(std::is_nothrow_destructible_v<Value>&& std::is_nothrow_destructible_v<Error>) =
        default;

    [[nodiscard]] constexpr bool has_value() const noexcept { return std::holds_alternative<Value>(value); }
    [[nodiscard]] constexpr bool has_error() const noexcept { return std::holds_alternative<Error>(value); }

    const constexpr Value& to_value() const { return std::get<Value>(value); }
    const constexpr Error& to_error() const { return std::get<Error>(value); }

    constexpr Value&& to_value() { return std::get<Value>(std::move(value)); }
    constexpr Error&& to_error() { return std::get<Error>(std::move(value)); }

    template<std::invocable<Value> F>
    const constexpr result& if_value(F&& f) const noexcept(std::is_nothrow_invocable_v<F, Value>)
    {
        std::visit(
            [&](auto&& arg)
            {
                using T = std::decay_t<decltype(arg)>;

                if constexpr (std::is_same_v<T, Value>) std::invoke(f, arg);
            },
            value);

        return *this;
    }

    template<std::invocable<Error> F>
    const constexpr result& if_error(F&& f) const noexcept(std::is_nothrow_invocable_v<F, Error>)
    {
        std::visit(
            [&](auto&& arg)
            {
                using T = std::decay_t<decltype(arg)>;

                if constexpr (std::is_same_v<T, Error>) std::invoke(f, arg);
            },
            value);
        return *this;
    }

    template<std::invocable<> F>
        requires std::is_same_v<std::invoke_result_t<F>, Value>
    constexpr Value or_else(F&& f) const noexcept(std::is_nothrow_invocable_v<F>)
    {
        if (has_error()) return std::invoke(f);
        return to_value();
    }

    constexpr Value or_else(Value&& other) const noexcept
    {
        if (has_error()) return other;
        return value;
    }

    template<std::invocable<Value> F>
    constexpr auto map(F&& f) const noexcept(std::is_nothrow_invocable_v<F, Value>)
    {
        return std::visit(
            [&](auto&& arg)
            {
                using T = std::decay_t<decltype(arg)>;

                if constexpr (std::is_same_v<T, Value>)
                {
                    auto r = std::invoke(f, arg);
                    return result<decltype(r), Error>{r};
                }
                else
                {
                    using R = std::invoke_result_t<F, decltype(arg)>;
                    return result<R, Error>{arg};
                }
            },
            value);
    }

private:
    std::variant<Value, Error> value;
};

}
