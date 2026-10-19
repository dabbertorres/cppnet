#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "http/middleware.hpp"
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
//
// Middleware (see http/middleware.hpp) runs around some routes only with use(), on a router or a subrouter:
//
//     router.subrouter("/api", [](http::router& api) { api.use(auth{}).GET("/users/:id", get_user); });
//
// It applies to every route of the router (or subrouter) it's used on, and of its subrouters: every route under its
// path, whether added before use() or after, through it or not (router.GET("/api/x", h) is under the "/api" subrouter
// too). It runs only for the routes, not for the router's 404s and 405s, which have none. A route's middleware runs
// outermost first: an outer router's before a subrouter's, and one router's in the order use() added it. Its handlers
// are type-erased (request_handlers), and so is the middleware: each layer is called through a std::function, with a
// next_handler, so a request through N layers allocates N coroutine frames more than its handler alone does (one per
// layer; Clang elides nothing it calls indirectly). The middleware is copied into the router once; one value used on
// several routers shares its state as its copies share it (e.g. by a shared_ptr). Middleware that can't be copied is
// held by a shared_ptr.
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

    // use adds middleware around every route of this router and its subrouters (see above), after (inside) what this
    // router has already.
    template<typename... M>
    router& use(M&&... layers)
    {
        static_assert((middleware<std::decay_t<M>, const next_handler> && ...),
                      "use() takes middleware: values with a const operator()(server_request&, response_writer&, "
                      "Next&) returning coro::task<void> (see http/middleware.hpp)");

        (add_layer(make_layer(std::forward<M>(layers))), ...);
        return *this;
    }

    // operator() calls the handler for the request's method and path, after setting its route to the pattern that
    // matched, or responds 404 Not Found (if no route matched) or 405 Method Not Allowed (if one did, but not for
    // its method).
    coro::task<void> operator()(server_request& /*req*/, response_writer& /*resp*/) const;

private:
    // layer is a middleware, type-erased: called with the request, its response and the rest of the route's chain.
    using layer     = std::function<coro::task<void>(server_request&, response_writer&, const next_handler&)>;
    using layer_ptr = std::shared_ptr<const layer>;

    // route_handler is a route's handler for one method, with the middleware it runs in, outermost first. It's held
    // by a unique_ptr, so that its steps (which next_handlers refer to) stay where they are as the router moves.
    class route_handler
    {
    public:
        route_handler(handler_func&& h, const std::vector<layer_ptr>& layers);

        route_handler(const route_handler&)            = delete;
        route_handler& operator=(const route_handler&) = delete;
        route_handler(route_handler&&)                 = delete;
        route_handler& operator=(route_handler&&)      = delete;
        ~route_handler()                               = default;

        // set_layers sets the middleware the handler runs in.
        void set_layers(const std::vector<layer_ptr>& from);

        // operator() runs the route: its outermost middleware, or its handler if it has none.
        coro::task<void> operator()(server_request& req, response_writer& resp) const { return steps[0](req, resp); }

    private:
        // step runs the route from its middleware at index on: that layer, or the handler past the last.
        struct step
        {
            const route_handler* owner;
            std::size_t          index;

            coro::task<void> operator()(server_request& req, response_writer& resp) const;
        };

        handler_func           handler;
        std::vector<layer_ptr> layers;
        // one per layer, then the handler's; and a next_handler referring to each, which the layer before is given.
        std::vector<step>         steps;
        std::vector<next_handler> nexts;
    };

    // make_layer type-erases middleware.
    template<typename M>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): it is, into the lambda's capture.
    static layer_ptr make_layer(M&& value)
    {
        using type = std::decay_t<M>;

        if constexpr (std::copy_constructible<type>)
        {
            return std::make_shared<const layer>(
                [m = type{std::forward<M>(value)}](server_request& req, response_writer& resp, const next_handler& next)
                { return m(req, resp, next); });
        }
        else
        {
            // held by a shared_ptr, as a std::function's target must be copyable.
            return std::make_shared<const layer>(
                [m = std::make_shared<const type>(
                     std::forward<M>(value))](server_request& req, response_writer& resp, const next_handler& next)
                { return (*m)(req, resp, next); });
        }
    }

    // add_layer adds middleware after this router's (and its parents'), to it and everything under it.
    void add_layer(const layer_ptr& l);

    // insert_layer inserts middleware at index in the layers of this router and everything under it.
    void insert_layer(std::size_t index, const layer_ptr& l);

    router& get_subrouter(std::string_view route);

    // allowed_methods lists the methods this route handles, for an Allow header.
    [[nodiscard]] std::string allowed_methods() const;

    std::unordered_map<request_method, std::unique_ptr<route_handler>> method_handlers;
    util::string_map<router>                                           children;
    std::unique_ptr<router>                                            wildcard;
    std::string                                                        wildcard_name;
    // pattern is the route to this router: e.g. "/users/:id".
    std::string pattern;
    // layers is the middleware this router's routes run in: its parents', then its own.
    std::vector<layer_ptr> layers;
};

}
