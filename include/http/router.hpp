#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "http/http.hpp"

namespace net::http
{

template<typename T>
concept HandlerInvocable = std::invocable<T, const request&, response&>;

template<typename T>
concept MatcherInvocable =
    std::invocable<T, const request&> && std::same_as<bool, std::invoke_result_t<T, const request&>>;

using handler = std::function<void(const request&, response&)>;
using matcher = std::function<bool(const request&)>;

struct route
{
    route() = delete;
    route(request_method method, std::string prefix, std::string suffix, matcher&& m, handler&& h) noexcept
        : prefix{std::move(prefix)}
        , suffix{std::move(suffix)}
        , method{method}
        , matcher{std::move(m)}
        , handler{std::move(h)}
    {}

    route(const route&) = delete;

    route(route&& other) noexcept
        : prefix{std::move(other.prefix)}
        , suffix{std::move(other.suffix)}
        , method{other.method}
        , matcher{std::move(other.matcher)}
        , handler{std::move(other.handler)}
    {}

    route& operator=(const route&) = delete;
    route& operator=(route&& other) noexcept
    {
        prefix  = std::move(other.prefix);
        suffix  = std::move(other.suffix);
        method  = other.method;
        matcher = std::move(other.matcher);
        handler = std::move(other.handler);

        return *this;
    }

    ~route() = default;

    std::string    prefix;
    std::string    suffix; // may be empty
    request_method method;

    std::function<bool(const request&)>            matcher;
    std::function<void(const request&, response&)> handler;
};

class router final
{
public:
    router& add(route&& route) noexcept;

    /* template<HandlerInvocable Handler> */
    /* router& add(matcher&& m, Handler&& func) noexcept */
    /* { */
    /*     return add(std::make_unique<matcher>(std::move(m)), std::forward<Handler>(func)); */
    /* } */

    /* template<MatcherInvocable Matcher> */
    /* router& add(Matcher&& m, handler&& h) noexcept */
    /* { */
    /*     return add(std::forward<Matcher>(m), std::make_unique<handler>(std::move(h))); */
    /* } */

    /* template<MatcherInvocable Matcher, HandlerInvocable Handler> */
    /* router& add(Matcher&& m, Handler&& h) noexcept */
    /* { */
    /*     return add(std::forward<Matcher>(m), std::forward<Handler>(h)); */
    /* } */

    void operator()(const request& req, response& resp);

private:
    std::vector<route> routes;
};

}
