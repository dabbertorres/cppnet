#pragma once

#include <concepts> // IWYU pragma: keep
#include <functional>
#include <type_traits>

namespace net::util
{

template<typename F>
    requires(std::is_nothrow_invocable_v<F>)
struct defer
{
public:
    explicit defer(F f) noexcept
        : deferred{std::move(f)}
    {}

    defer(const defer&)            = delete;
    defer& operator=(const defer&) = delete;

    defer(defer&& other) noexcept
        : deferred{std::move(other.deferred)}
        , invoke{std::exchange(other.invoke, false)}
    {}

    defer& operator=(defer&&) noexcept = delete;

    ~defer() noexcept
    {
        if (invoke) std::invoke(deferred);
    }

private:
    F    deferred;
    bool invoke = true;
};

}
