#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace net::util
{

template<class Signature>
class function_ref;

// function_ref refers to something callable without owning it, as C++26's std::function_ref does: two pointers, the
// object and a function that calls it, so passing one never allocates (as a std::function may). Unlike
// std::function_ref, it may be empty (made by its default constructor), which operator bool tells, and calling an empty
// one is undefined. It can't be made from a temporary, which would be gone before it's called (e.g. by a coroutine
// that takes it as a parameter): what it refers to must outlive it.
template<class R, class... Args>
class function_ref<R(Args...)>
{
public:
    function_ref() noexcept = default;

    // function_ref refers to f, which must outlive it.
    template<class F>
        requires(!std::same_as<std::remove_cv_t<F>, function_ref>) && std::is_invocable_r_v<R, const F&, Args...>
    // NOLINTNEXTLINE(*-explicit-constructor): converts, as a parameter.
    function_ref(const F& f) noexcept
        : object{std::addressof(f)}
        , call{[](const void* referred, Args... args) -> R
               { return std::invoke(*static_cast<const F*>(referred), std::forward<Args>(args)...); }}
    {}

    template<class F>
    function_ref(const F&&) = delete;

    R operator()(Args... args) const { return call(object, std::forward<Args>(args)...); }

    explicit operator bool() const noexcept { return call != nullptr; }

private:
    const void* object              = nullptr;
    R (*call)(const void*, Args...) = nullptr;
};

}
