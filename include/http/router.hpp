#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "http/handler.hpp"
#include "http/http.hpp"

namespace net::http
{

// NOTE: clangd seems to think Handler isn't being used - so make it explicit.
using net::http::Handler;

template<typename T>
concept Matcher = std::invocable<T, const request&> && std::same_as<bool, std::invoke_result_t<T, const request&>>;

struct route
{
    route() = delete;

    template<Handler H, Matcher M>
    route(request_method method, std::string prefix, std::string suffix, H&& handler, M&& matcher) noexcept
        : prefix{std::move(prefix)}
        , suffix{std::move(suffix)}
        , method{method}
        , matcher{std::forward<H>(handler)}
        , handler{std::forward<M>(matcher)}
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

    std::function<bool(const request&)>                   matcher;
    std::function<void(const request&, server_response&)> handler;
};

class router final
{
public:
    router& add(route&& route) noexcept
    {
        routes.emplace_back(std::move(route));
        return *this;
    }

    template<typename... Args>
    router& add(Args&&... args) noexcept
    {
        routes.emplace_back(std::forward<Args...>(args...));
        return *this;
    }

    void operator()(const request& req, server_response& resp) const
    {
        for (const auto& r : routes)
        {
            if (r.matcher(req))
            {
                r.handler(req, resp);
                return;
            }
        }
    }

private:
    std::vector<route> routes;
};

}
