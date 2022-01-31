#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "http/http.hpp"

namespace net::http
{

template<typename T>
concept HandlerFunc = requires(T& t, const request& req, response& resp)
{
    t(req, resp);
};

struct handler
{
    virtual ~handler() = default;
    virtual void handle(const request&, response&) = 0;
};

struct matcher
{
    virtual ~matcher() = default;
    virtual bool match(const request&) = 0;
};

class router final : public handler
{
public:
    router& route(method m, std::string_view path, std::unique_ptr<handler> h) noexcept;

    template<HandlerFunc Func>
    router& route(method m, std::string_view path, Func&& func) noexcept
    {
        auto match = std::make_unique<method_path_matcher>(m, path);
        auto handle = std::make_unique<handler_func>(func);
        return route(std::move(match), std::move(handle));
    }

    template<HandlerFunc Func>
    router& route(std::unique_ptr<matcher> m, Func&& func) noexcept
    {
        auto handle = std::make_unique<handler_func>(func);
        return route(m, std::move(handle));
    }

    router& route(std::unique_ptr<matcher> m, std::unique_ptr<handler> h) noexcept;

    void handle(const request&, response&) override final;

private:
    struct method_path_matcher : public matcher
    {
        method_path_matcher(method m, std::string_view p)
            : method{m}, path{p}
        {}

        const method method;
        const std::string path;

        bool match(const request& req) override final
        {
            return req.path == path && method == req.method;
        }
    };

    struct handler_func : public handler
    {
        const std::function<void(const request&, response&)> func;

        void handle(const request& req, response& resp) override final
        {
            func(req, resp);
        }
    };

    struct route_matcher
    {
        route_matcher() = delete;
        route_matcher(std::unique_ptr<matcher>&&, std::unique_ptr<handler>&&) noexcept;
        route_matcher(const route_matcher&) = delete;
        route_matcher(route_matcher&&) noexcept;

        route_matcher& operator=(const route_matcher&) = delete;
        route_matcher& operator=(route_matcher&&) noexcept;

        ~route_matcher() = default;

        std::unique_ptr<matcher> matcher;
        std::unique_ptr<handler> handler;
    };

    std::vector<route_matcher> routes;
};

}
