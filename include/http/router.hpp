#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "util/string_map.hpp"

namespace net::http
{

using handler_func = std::function<coro::task<void>(const server_request&, response_writer&)>;

// TODO: it'd be neat if we could build a "perfect" router at compile time...

class router final
{
public:
    router& GET(std::string_view route, handler_func&& h)
    {
        return add(request_method::GET, route, std::forward<handler_func>(h));
    }

    router& POST(std::string_view route, handler_func&& h)
    {
        return add(request_method::POST, route, std::forward<handler_func>(h));
    }

    router& DELETE(std::string_view route, handler_func&& h)
    {
        return add(request_method::DELETE, route, std::forward<handler_func>(h));
    }

    router& PUT(std::string_view route, handler_func&& h)
    {
        return add(request_method::PUT, route, std::forward<handler_func>(h));
    }

    router& PATCH(std::string_view route, handler_func&& h)
    {
        return add(request_method::PATCH, route, std::forward<handler_func>(h));
    }

    router& HEAD(std::string_view route, handler_func&& h)
    {
        return add(request_method::HEAD, route, std::forward<handler_func>(h));
    }

    router& CONNECT(std::string_view route, handler_func&& h)
    {
        return add(request_method::CONNECT, route, std::forward<handler_func>(h));
    }

    router& TRACE(std::string_view route, handler_func&& h)
    {
        return add(request_method::TRACE, route, std::forward<handler_func>(h));
    }

    router& OPTIONS(std::string_view route, handler_func&& h)
    {
        return add(request_method::OPTIONS, route, std::forward<handler_func>(h));
    }

    router& add(request_method method, std::string_view route, handler_func&& h);

    template<std::invocable<router&> I>
    router& subrouter(std::string_view route, I&& sub_builder)
    {
        auto& sub = get_subrouter(route);
        std::invoke(std::forward<I>(sub_builder), sub);
    }

    coro::task<void> operator()(const server_request&, response_writer&) const;

private:
    router& get_subrouter(std::string_view route);

    std::unordered_map<request_method, handler_func> method_handlers;
    util::string_map<router>                         children;
    std::unique_ptr<router>                          wildcard;
    std::string                                      wildcard_name;
};

}
