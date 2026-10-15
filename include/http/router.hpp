#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "util/string_map.hpp"

namespace net::http
{

using handler_func = std::function<coro::task<void>(const server_request&, response_writer&)>;

// request_handler is what a server calls for each request: e.g. a router, or middleware wrapping one (see
// instrument/middleware.hpp). Unlike a handler_func, it can change the request (e.g. a router sets its route).
using request_handler = std::function<coro::task<void>(server_request&, response_writer&)>;

// make_handler makes a request_handler of h, which needn't be copyable (as a router isn't): copies of the
// request_handler share it.
template<typename H>
    requires std::invocable<std::decay_t<H>&, server_request&, response_writer&>
request_handler make_handler(H&& h)
{
    auto shared = std::make_shared<std::decay_t<H>>(std::forward<H>(h));
    return [shared](server_request& req, response_writer& resp) { return std::invoke(*shared, req, resp); };
}

// TODO: it'd be neat if we could build a "perfect" router at compile time...

class router final
{
public:
    router& GET(std::string_view route, handler_func&& h) { return add(request_method::GET, route, std::move(h)); }

    router& POST(std::string_view route, handler_func&& h) { return add(request_method::POST, route, std::move(h)); }

    router& DELETE(std::string_view route, handler_func&& h)
    {
        return add(request_method::DELETE, route, std::move(h));
    }

    router& PUT(std::string_view route, handler_func&& h) { return add(request_method::PUT, route, std::move(h)); }

    router& PATCH(std::string_view route, handler_func&& h) { return add(request_method::PATCH, route, std::move(h)); }

    router& HEAD(std::string_view route, handler_func&& h) { return add(request_method::HEAD, route, std::move(h)); }

    router& CONNECT(std::string_view route, handler_func&& h)
    {
        return add(request_method::CONNECT, route, std::move(h));
    }

    router& TRACE(std::string_view route, handler_func&& h) { return add(request_method::TRACE, route, std::move(h)); }

    router& OPTIONS(std::string_view route, handler_func&& h)
    {
        return add(request_method::OPTIONS, route, std::move(h));
    }

    router& add(request_method method, std::string_view route, handler_func&& h);

    template<std::invocable<router&> I>
    router& subrouter(std::string_view route, I&& sub_builder)
    {
        auto& sub = get_subrouter(route);
        std::invoke(std::forward<I>(sub_builder), sub);
        return *this;
    }

    // operator() calls the handler for the request's method and path, after setting its route to the pattern that
    // matched, or responds 404 Not Found (if no route matched) or 405 Method Not Allowed (if one did, but not for
    // its method).
    coro::task<void> operator()(server_request&, response_writer&) const;

private:
    router& get_subrouter(std::string_view route);

    std::unordered_map<request_method, handler_func> method_handlers;
    util::string_map<router>                         children;
    std::unique_ptr<router>                          wildcard;
    std::string                                      wildcard_name;
    // pattern is the route to this router: e.g. "/users/:id".
    std::string pattern;
};

}
