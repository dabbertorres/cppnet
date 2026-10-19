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
#include "http/route_pattern.hpp"
#include "util/string_map.hpp"

namespace net::http
{

// request_handler is what a server calls for each request: e.g. a router, or a chain of middleware around one (see
// http/middleware.hpp), or a route's handler. It may take the request as const (most handlers only read it), or not
// (e.g. one that adds to its extensions).
using request_handler = std::function<coro::task<void>(server_request&, response_writer&)>;

// handler_func is a route's handler (see router): a request_handler.
using handler_func = request_handler;

// make_handler makes a request_handler of h, which needn't be copyable (as a router isn't): copies of the
// request_handler share it.
template<typename H>
    requires std::invocable<std::decay_t<H>&, server_request&, response_writer&>
request_handler make_handler(H&& h)
{
    auto shared = std::make_shared<std::decay_t<H>>(std::forward<H>(h));
    return [shared](server_request& req, response_writer& resp) { return std::invoke(*shared, req, resp); };
}

// move_only_handler is a handler that can be moved but not copied (e.g. a router, or a chain holding one: see
// http/middleware.hpp), as an rvalue: it can't be a request_handler itself (a std::function, which copies what it
// holds), but make_handler can make one of it, as the server's constructors do.
template<typename H>
concept move_only_handler = !std::is_reference_v<H> && std::move_constructible<H> && !std::copy_constructible<H>
                         && std::is_invocable_r_v<coro::task<void>, H&, server_request&, response_writer&>;

// router routes requests to handlers by method and path.
//
// A route is a path of segments separated by '/'. A segment starting with ':' is a parameter (e.g. "/users/:id"),
// which matches any one non-empty segment; handlers get its value from server_request::path_value. A literal segment
// is matched in preference to a parameter, and a request whose path then leads nowhere isn't tried with the parameter
// instead: e.g. with "/users/me/settings" and "/users/:id/posts", "/users/me/posts" isn't found.
//
// A route's GET handler also handles HEAD, unless the route has its own (the server drops the body).
class router final
{
public:
    router& GET(route_pattern route, handler_func&& h) { return add(request_method::GET, route, std::move(h)); }

    router& POST(route_pattern route, handler_func&& h) { return add(request_method::POST, route, std::move(h)); }

    router& DELETE(route_pattern route, handler_func&& h) { return add(request_method::DELETE, route, std::move(h)); }

    router& PUT(route_pattern route, handler_func&& h) { return add(request_method::PUT, route, std::move(h)); }

    router& PATCH(route_pattern route, handler_func&& h) { return add(request_method::PATCH, route, std::move(h)); }

    router& HEAD(route_pattern route, handler_func&& h) { return add(request_method::HEAD, route, std::move(h)); }

    router& CONNECT(route_pattern route, handler_func&& h) { return add(request_method::CONNECT, route, std::move(h)); }

    router& TRACE(route_pattern route, handler_func&& h) { return add(request_method::TRACE, route, std::move(h)); }

    router& OPTIONS(route_pattern route, handler_func&& h) { return add(request_method::OPTIONS, route, std::move(h)); }

    router& add(request_method method, route_pattern route, handler_func&& h);

    template<std::invocable<router&> I>
    router& subrouter(route_pattern route, I&& sub_builder)
    {
        auto& sub = get_subrouter(route.view());
        std::invoke(std::forward<I>(sub_builder), sub);
        return *this;
    }

    // operator() calls the handler for the request's method and path, after setting its route to the pattern that
    // matched, or responds 404 Not Found (if no route matched) or 405 Method Not Allowed (if one did, but not for
    // its method).
    coro::task<void> operator()(server_request& /*req*/, response_writer& /*resp*/) const;

private:
    router& get_subrouter(std::string_view route);

    // allowed_methods lists the methods this route handles, for an Allow header.
    [[nodiscard]] std::string allowed_methods() const;

    std::unordered_map<request_method, handler_func> method_handlers;
    util::string_map<router>                         children;
    std::unique_ptr<router>                          wildcard;
    std::string                                      wildcard_name;
    // pattern is the route to this router: e.g. "/users/:id".
    std::string pattern;
};

}
