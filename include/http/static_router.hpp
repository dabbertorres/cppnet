#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "http/middleware.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/route_pattern.hpp"
#include "util/fixed_string.hpp"
#include "util/small_vector.hpp"

// static_router is a router whose routes are known at compile time: their patterns are template arguments, so that
// the routing tree is built while compiling, as tables (each node's literal segments in a perfect hash table), and
// handlers are called through a table of function pointers. Mistakes between routes (the same route twice, or two
// names for a parameter) fail to compile, and handlers can ask for their parameters by name, checked at compile time.
//
// It routes as router does (see router): literal segments win over parameters, without backtracking; a parameter
// never matches an empty segment; GET handles HEAD, unless a route has its own; and 405 responses list the allowed
// methods. It sets server_request::route and path_values too.
//
//     using net::http::literals::operator""_route;
//
//     auto routes = make_static_router(
//         GET("/users/:id"_route,
//             [](const server_request& req, response_writer& resp, params<"/users/:id"> p) -> task<void>
//             { ... p.get<"id">() ... }),
//         group("/api"_route, GET("/health"_route, health)));
//
// Only the patterns need to be known at compile time: handlers are values (e.g. lambdas with captures).
//
// Middleware (see http/middleware.hpp) runs around some routes only with use(), first in a group: it applies to every
// route in the group, and in the groups within it, but not to the group's 404s and 405s, which have no route:
//
//     auto routes = make_static_router(
//         GET("/health"_route, health),
//         group("/api"_route, use(auth{}, limits{}),
//               GET("/users/:id"_route, get_user),
//               group("/admin"_route, use(audit{}), POST("/reset"_route, reset))));
//
// A route's middleware is composed with its handler at compile time, as chain() composes it: its outermost group's
// middleware runs first (auth, limits, then audit, for /api/admin/reset), and the innermost layer calls the handler
// (with its params, if it takes them) directly, so that Clang puts the layers' and the handler's coroutine frames in
// the outermost layer's: middleware on routes costs no allocation per request over the handler alone. (A chain around
// the whole router costs one, as a router's handlers aren't elided into the layer that calls it. So does a handler
// given as a function, which is a pointer, called indirectly: a lambda calling the function is elided.) use() first in
// make_static_router applies to every route. Each route holds its own copies of its middleware (one value used on
// several routes is copied to each, so its state is shared as its copies share it, e.g. by a shared_ptr), made once,
// with the router: middleware that can't be copied can be used on one route only.

namespace net::http
{

// route is a route's pattern, as a type: see the _route literal. A bad pattern fails to compile (see route_pattern).
template<util::fixed_string Pattern>
struct route
{
    static constexpr auto          pattern = Pattern;
    static constexpr route_pattern checked{Pattern.view()};
};

inline namespace literals
{

    // "/users/:id"_route is route<"/users/:id">{}.
    template<util::fixed_string Pattern>
    consteval route<Pattern> operator""_route() noexcept
    {
        return {};
    }

}

namespace detail
{

    inline constexpr std::size_t no_parameter = std::numeric_limits<std::size_t>::max();

    // parameter_count counts pattern's parameters.
    constexpr std::size_t parameter_count(std::string_view pattern) noexcept
    {
        std::size_t count = 0;
        for (std::size_t i = 0; i + 1 < pattern.size(); ++i)
            if (pattern[i] == '/' && pattern[i + 1] == ':') ++count;
        return count;
    }

    // parameter_index is the position of the parameter name among pattern's parameters, or no_parameter.
    // NOLINTNEXTLINE(bugprone-exception-escape): substr's positions are in range.
    constexpr std::size_t parameter_index(std::string_view pattern, std::string_view name) noexcept
    {
        std::size_t index = 0;
        for (std::size_t start = 1; start <= pattern.size();)
        {
            const auto end     = segment_end(pattern, start);
            const auto segment = pattern.substr(start, end - start);
            start              = end + 1;

            if (!segment.starts_with(':')) continue;
            if (segment.substr(1) == name) return index;
            ++index;
        }

        return no_parameter;
    }

}

// params gives a handler its route's parameters, by name: e.g. p.get<"id">() for "/users/:id". A name the pattern
// doesn't have fails to compile.
//
// Handlers take it by value (it's a pointer and an index): it's made for the call, which returns the handler's task
// before it runs. It views the request, so mustn't outlive it. A route's params convert to those of a route it ends
// with, so that a handler written for "/users/:id" can take them when it's in a group under a prefix.
template<util::fixed_string Pattern>
class params
{
public:
    explicit params(const server_request& req) noexcept
        : req{&req}
    {}

    // A handler in a group takes its own route's params.
    template<util::fixed_string Full>
        requires(Full.view().ends_with(Pattern.view()) && !std::is_same_v<params<Full>, params>)
    // NOLINTNEXTLINE(*-explicit-constructor): converts implicitly, as the handler's parameter.
    params(const params<Full>& full) noexcept
        : req{full.request()}
        , offset{detail::parameter_count(Full.view()) - detail::parameter_count(Pattern.view())}
    {}

    template<util::fixed_string Name>
        requires(detail::parameter_index(Pattern.view(), Name.view()) != detail::no_parameter)
    [[nodiscard]] std::string_view get() const noexcept
    {
        constexpr auto index = detail::parameter_index(Pattern.view(), Name.view());
        return req->path_values[offset + index].second;
    }

    [[nodiscard]] const server_request* request() const noexcept { return req; }

private:
    const server_request* req;
    std::size_t           offset = 0;
};

namespace detail
{

    // uses is the middleware use() gives a group.
    template<typename... Middleware>
    struct uses
    {
        std::tuple<Middleware...> layers;
    };

}

// use is middleware for a group's routes (see the top of this header): the first argument of group(), or of
// make_static_router(). The first middleware runs outermost.
template<typename... Middleware>
constexpr detail::uses<std::decay_t<Middleware>...> use(Middleware&&... layers)
{
    static_assert((middleware<std::decay_t<Middleware>> && ...),
                  "use() takes middleware: values with a const operator()(server_request&, response_writer&, Next&) "
                  "returning coro::task<void> (see http/middleware.hpp)");

    return {{std::forward<Middleware>(layers)...}};
}

// route_entry is one route: a method and pattern, and the handler for them.
template<request_method Method, util::fixed_string Pattern, typename Handler>
struct route_entry
{
    static constexpr request_method method  = Method;
    static constexpr auto           pattern = Pattern;

    Handler handler;
};

template<request_method Method, util::fixed_string Pattern, typename Handler>
constexpr route_entry<Method, Pattern, std::decay_t<Handler>> handle(route<Pattern> /*unused*/, Handler&& handler)
{
    return {std::forward<Handler>(handler)};
}

// NOLINTBEGIN(readability-identifier-naming): named as router's methods are.
template<util::fixed_string P, typename H>
constexpr auto GET(route<P> r, H&& h)
{
    return handle<request_method::GET>(r, std::forward<H>(h));
}

template<util::fixed_string P, typename H>
constexpr auto POST(route<P> r, H&& h)
{
    return handle<request_method::POST>(r, std::forward<H>(h));
}

template<util::fixed_string P, typename H>
constexpr auto PUT(route<P> r, H&& h)
{
    return handle<request_method::PUT>(r, std::forward<H>(h));
}

template<util::fixed_string P, typename H>
constexpr auto PATCH(route<P> r, H&& h)
{
    return handle<request_method::PATCH>(r, std::forward<H>(h));
}

template<util::fixed_string P, typename H>
constexpr auto DELETE(route<P> r, H&& h)
{
    return handle<request_method::DELETE>(r, std::forward<H>(h));
}

template<util::fixed_string P, typename H>
constexpr auto HEAD(route<P> r, H&& h)
{
    return handle<request_method::HEAD>(r, std::forward<H>(h));
}

template<util::fixed_string P, typename H>
constexpr auto OPTIONS(route<P> r, H&& h)
{
    return handle<request_method::OPTIONS>(r, std::forward<H>(h));
}

template<util::fixed_string P, typename H>
constexpr auto TRACE(route<P> r, H&& h)
{
    return handle<request_method::TRACE>(r, std::forward<H>(h));
}

template<util::fixed_string P, typename H>
constexpr auto CONNECT(route<P> r, H&& h)
{
    return handle<request_method::CONNECT>(r, std::forward<H>(h));
}
// NOLINTEND(readability-identifier-naming)

namespace detail
{

    template<typename T>
    struct is_route_entry : std::false_type
    {};

    template<request_method M, util::fixed_string P, typename H>
    struct is_route_entry<route_entry<M, P, H>> : std::true_type
    {};

    template<typename T>
    struct is_uses : std::false_type
    {};

    template<typename... Middleware>
    struct is_uses<uses<Middleware...>> : std::true_type
    {};

    // layered is a route's handler with the middleware it runs in (from use()), outermost first.
    template<typename Layers, typename Handler>
    struct layered
    {
        Layers  layers;
        Handler handler;
    };

    template<typename T>
    struct is_layered : std::false_type
    {};

    template<typename Layers, typename Handler>
    struct is_layered<layered<Layers, Handler>> : std::true_type
    {};

    // flatten makes one tuple of route entries, from entries and tuples of them (from group).
    template<typename... Args>
    constexpr auto flatten(Args&&... args)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): it is, in either branch.
        const auto as_tuple = []<typename A>(A&& arg)
        {
            if constexpr (is_route_entry<std::decay_t<A>>::value)
            {
                return std::tuple<std::decay_t<A>>{std::forward<A>(arg)};
            }
            else if constexpr (is_uses<std::decay_t<A>>::value)
            {
                static_assert(!is_uses<std::decay_t<A>>::value,
                              "use() comes first in a group (or in make_static_router), before its routes");
                return std::tuple<>{};
            }
            else
            {
                return std::forward<A>(arg);
            }
        };

        return std::tuple_cat(as_tuple(std::forward<Args>(args))...);
    }

    // add_layers puts layers around entry's handler, outside any it has already (from a group within).
    template<typename... Middleware, request_method M, util::fixed_string P, typename H>
    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved): its handler is moved from.
    constexpr auto add_layers(std::tuple<Middleware...>&& layers, route_entry<M, P, H>&& entry)
    {
        if constexpr (is_layered<H>::value)
        {
            auto all = std::tuple_cat(std::move(layers), std::move(entry.handler.layers));
            return route_entry<M, P, layered<decltype(all), decltype(entry.handler.handler)>>{
                {.layers = std::move(all), .handler = std::move(entry.handler.handler)}
            };
        }
        else
        {
            return route_entry<M, P, layered<std::tuple<Middleware...>, H>>{
                {.layers = std::move(layers), .handler = std::move(entry.handler)}
            };
        }
    }

    // layers_for is the middleware for one of a group's routes: a copy, or (for the last) the middleware itself.
    template<bool Last, typename Layers>
    constexpr Layers layers_for(Layers& layers)
    {
        if constexpr (Last) return std::move(layers);
        else return layers;
    }

    // apply_uses puts used's middleware around each of entries' handlers.
    template<typename... Middleware, typename... Entries>
    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved): its entries are moved from.
    constexpr auto apply_uses(uses<Middleware...> used, std::tuple<Entries...>&& entries)
    {
        constexpr auto count = sizeof...(Entries);

        static_assert(count <= 1 || (std::copy_constructible<Middleware> && ...),
                      "use() copies its middleware to each of the group's routes: middleware that can't be copied can "
                      "be used on one route only");

        return [&]<std::size_t... I>(std::index_sequence<I...>)
        {
            // NOTE: a braced list is evaluated in order, so the last route's layers, moved from used, are taken last.
            return std::tuple{add_layers(layers_for<I + 1 == count>(used.layers), std::move(std::get<I>(entries)))...};
        }(std::index_sequence_for<Entries...>{});
    }

    // flatten_group is flatten for a group's (or make_static_router's) arguments: the first may be use()'s
    // middleware, which is put around each of the rest's routes.
    constexpr std::tuple<> flatten_group() { return {}; }

    template<typename First, typename... Rest>
    constexpr auto flatten_group(First&& first, Rest&&... rest)
    {
        if constexpr (is_uses<std::decay_t<First>>::value)
            return apply_uses(std::decay_t<First>{std::forward<First>(first)}, flatten(std::forward<Rest>(rest)...));
        else return flatten(std::forward<First>(first), std::forward<Rest>(rest)...);
    }

    template<util::fixed_string Prefix, request_method M, util::fixed_string P, typename H>
    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved): its handler is moved from.
    constexpr route_entry<M, Prefix + P, H> prefixed(route_entry<M, P, H>&& entry)
    {
        return {std::move(entry.handler)};
    }

}

// group prefixes routes (or other groups) with prefix: e.g. group("/api"_route, GET("/health"_route, h)) is
// GET("/api/health"_route, h). Its first argument may be use(...): middleware for each of its routes (see the top of
// this header).
template<util::fixed_string Prefix, typename... Entries>
constexpr auto group(route<Prefix> /*unused*/, Entries&&... entries)
{
    return std::apply([](auto&&... flat) { return std::tuple{detail::prefixed<Prefix>(std::move(flat))...}; },
                      detail::flatten_group(std::forward<Entries>(entries)...));
}

namespace detail
{

    inline constexpr std::size_t   method_count = static_cast<std::size_t>(request_method::NONE) + 1;
    inline constexpr std::uint32_t none         = std::numeric_limits<std::uint32_t>::max();

    struct route_spec
    {
        request_method   method;
        std::string_view pattern;
    };

    // node is a point in the routing tree: where a path's segments so far lead.
    struct node
    {
        // the node's literal children, as a perfect hash table: slots [first_slot, first_slot + slot_count), indexed by
        // a segment's hash (with seed) modulo slot_count, which is a power of two.
        std::uint32_t seed       = 0;
        std::uint32_t first_slot = 0;
        std::uint32_t slot_count = 0;

        // the child for a parameter, if there is one.
        std::uint32_t    param_child = none;
        std::string_view param_name;

        // the route for each method, if any, and the pattern that leads here (for server_request::route).
        std::array<std::uint32_t, method_count> routes{};
        std::string_view                        pattern;
    };

    struct slot
    {
        std::string_view segment;
        std::uint32_t    child = none;
    };

    // mix finishes a hash, so that its low bits (which index the slots) depend on all of it.
    constexpr std::uint32_t mix(std::uint32_t h) noexcept
    {
        h ^= h >> 16;
        h *= 0x7feb'352dU;
        h ^= h >> 15;
        h *= 0x846c'a68bU;
        h ^= h >> 16;
        return h;
    }

    constexpr std::uint32_t hash_start(std::uint32_t seed) noexcept { return 2'166'136'261U ^ (seed * 0x9e37'79b9U); }

    constexpr std::uint32_t hash_byte(std::uint32_t h, char c) noexcept
    {
        return (h ^ static_cast<unsigned char>(c)) * 16'777'619U;
    }

    // hash_literal hashes a route's literal segment, as it's written.
    constexpr std::uint32_t hash_literal(std::string_view segment, std::uint32_t seed) noexcept
    {
        auto h = hash_start(seed);
        for (char c : segment) h = hash_byte(h, c);
        return mix(h);
    }

    enum class routes_error : std::uint8_t
    {
        none,
        duplicate_route,       // two routes have the same method and pattern.
        conflicting_parameter, // two routes name a parameter differently, in the same place.
    };

    // build_tree builds the routing tree, as nodes (the root first), with each node's literal children not yet hashed.
    struct tree_node
    {
        std::vector<std::pair<std::string_view, std::uint32_t>> literals;
        std::uint32_t                                           param_child = none;
        std::string_view                                        param_name;
        std::array<std::uint32_t, method_count>                 routes{};
        std::string_view                                        pattern;
    };

    struct tree
    {
        std::vector<tree_node> nodes;
        routes_error           error = routes_error::none;
    };

    // descend returns the child of t's node current for segment, adding it if need be, or none if it conflicts.
    constexpr std::uint32_t descend(tree& t, std::uint32_t current, std::string_view segment)
    {
        const auto add = [&t]
        {
            const auto child = static_cast<std::uint32_t>(t.nodes.size());
            t.nodes.emplace_back().routes.fill(none);
            return child;
        };

        if (segment.starts_with(':'))
        {
            const auto name = segment.substr(1);
            if (t.nodes[current].param_child == none)
            {
                const auto child             = add();
                t.nodes[current].param_child = child;
                t.nodes[current].param_name  = name;
            }

            return t.nodes[current].param_name == name ? t.nodes[current].param_child : none;
        }

        for (const auto& [literal, child] : t.nodes[current].literals)
            if (literal == segment) return child;

        const auto child = add();
        t.nodes[current].literals.emplace_back(segment, child);
        return child;
    }

    constexpr tree build_tree(std::span<const route_spec> specs)
    {
        tree t;
        t.nodes.emplace_back().routes.fill(none);

        for (std::uint32_t r = 0; r < specs.size(); ++r)
        {
            const auto    pattern = specs[r].pattern;
            std::uint32_t current = 0;

            for (std::size_t start = 1; start <= pattern.size();)
            {
                const auto end = segment_end(pattern, start);

                current = descend(t, current, pattern.substr(start, end - start));
                start   = end + 1;

                if (current == none)
                {
                    t.error = routes_error::conflicting_parameter;
                    return t;
                }
            }

            auto& route = t.nodes[current].routes[static_cast<std::size_t>(specs[r].method)];
            if (route != none)
            {
                t.error = routes_error::duplicate_route;
                return t;
            }

            route                    = r;
            t.nodes[current].pattern = pattern;
        }

        return t;
    }

    // check_routes checks routes against each other (each pattern having been checked on its own, by route).
    constexpr routes_error check_routes(std::span<const route_spec> specs) { return build_tree(specs).error; }

    struct hash_layout
    {
        std::uint32_t seed = 0;
        std::uint32_t size = 0;
    };

    // perfect_hash finds a seed (and a table size) for which literals' hashes don't collide.
    constexpr hash_layout perfect_hash(const std::vector<std::pair<std::string_view, std::uint32_t>>& literals)
    {
        if (literals.empty()) return {};

        for (auto size = std::bit_ceil(static_cast<std::uint32_t>(literals.size()));; size *= 2)
        {
            for (std::uint32_t seed = 0; seed < 64; ++seed)
            {
                std::vector<char> used(size, 0);

                bool collided = false;
                for (const auto& [segment, _] : literals)
                {
                    auto& slot = used[hash_literal(segment, seed) & (size - 1)];
                    collided   = collided || slot != 0;
                    slot       = 1;
                }

                if (!collided) return {.seed = seed, .size = size};
            }
        }
    }

    struct table_sizes
    {
        std::size_t nodes = 0;
        std::size_t slots = 0;
    };

    constexpr table_sizes table_size(std::span<const route_spec> specs)
    {
        const auto t = build_tree(specs);

        table_sizes sizes{.nodes = t.nodes.size(), .slots = 0};
        for (const auto& n : t.nodes) sizes.slots += perfect_hash(n.literals).size;
        return sizes;
    }

    template<std::size_t Nodes, std::size_t Slots>
    struct table
    {
        std::array<node, Nodes> nodes{};
        std::array<slot, Slots> slots{};
    };

    template<std::size_t Nodes, std::size_t Slots>
    constexpr table<Nodes, Slots> build_table(std::span<const route_spec> specs)
    {
        const auto t = build_tree(specs);

        table<Nodes, Slots> out;
        std::uint32_t       next_slot = 0;

        for (std::size_t i = 0; i < Nodes; ++i)
        {
            const auto& from   = t.nodes[i];
            auto&       to     = out.nodes[i];
            const auto  layout = perfect_hash(from.literals);

            to.seed        = layout.seed;
            to.first_slot  = next_slot;
            to.slot_count  = layout.size;
            to.param_child = from.param_child;
            to.param_name  = from.param_name;
            to.routes      = from.routes;
            to.pattern     = from.pattern;

            for (const auto& [segment, child] : from.literals)
                out.slots[next_slot + (hash_literal(segment, layout.seed) & (layout.size - 1))] = {.segment = segment,
                                                                                                   .child   = child};

            next_slot += layout.size;
        }

        return out;
    }

    // match walks req's path through the tree. If it leads to a node with routes, it returns that node, along with the
    // route for req's method (or none), having added the parameters to req's path_values.
    struct match_result
    {
        const node*   found = nullptr;
        std::uint32_t route = none;
    };

    match_result match(std::span<const node> nodes, std::span<const slot> slots, server_request& req);

    // handler_signature finds the type a handler takes its params as, if its signature can be inspected: it can't for
    // e.g. a generic lambda.
    template<typename F>
    struct handler_signature
    {};

    template<typename R, typename A, typename B, typename P>
    struct handler_signature<R (*)(A, B, P)>
    {
        using params_type = P;
    };

    template<typename R, typename A, typename B, typename P>
    struct handler_signature<R (*)(A, B, P) noexcept>
    {
        using params_type = P;
    };

    template<typename R, typename C, typename A, typename B, typename P>
    struct handler_signature<R (C::*)(A, B, P) const>
    {
        using params_type = P;
    };

    template<typename R, typename C, typename A, typename B, typename P>
    struct handler_signature<R (C::*)(A, B, P) const noexcept>
    {
        using params_type = P;
    };

    template<typename R, typename C, typename A, typename B, typename P>
    struct handler_signature<R (C::*)(A, B, P)>
    {
        using params_type = P;
    };

    template<typename R, typename C, typename A, typename B, typename P>
    struct handler_signature<R (C::*)(A, B, P) noexcept>
    {
        using params_type = P;
    };

    template<typename H>
    struct handler_traits : handler_signature<H>
    {};

    template<typename H>
        requires requires { &H::operator(); }
    struct handler_traits<H> : handler_signature<decltype(&H::operator())>
    {};

    // takes_params_by_reference is whether a handler (that can be inspected) takes its params by reference.
    template<typename H>
    concept takes_params_by_reference = requires { typename handler_traits<H>::params_type; }
                                     && std::is_reference_v<typename handler_traits<H>::params_type>;

    // route_call calls a route's handler, with the route's params if it takes them. It isn't a coroutine: calling it
    // calls the handler.
    template<util::fixed_string Pattern, typename Handler>
    struct route_call
    {
        Handler handler;

        coro::task<void> operator()(server_request& req, response_writer& resp) const
        {
            if constexpr (std::invocable<const Handler&, server_request&, response_writer&, params<Pattern>>)
            {
                // the params are made for the call, which returns the handler's task before it runs: a reference
                // would dangle.
                static_assert(!takes_params_by_reference<Handler>,
                              "a handler takes its params by value (they're a pointer and an index)");

                return handler(req, resp, params<Pattern>{req});
            }
            else
            {
                static_assert(std::invocable<const Handler&, server_request&, response_writer&>,
                              "a route's handler is called with (server_request&, response_writer&), and its route's "
                              "params if it takes them");

                return std::invoke(handler, req, resp);
            }
        }
    };

    // make_route makes what the router calls for a route: its handler, or a chain of its middleware around it.
    template<request_method M, util::fixed_string P, typename H>
    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved): its handler is moved from.
    constexpr auto make_route(route_entry<M, P, H>&& entry)
    {
        if constexpr (is_layered<H>::value)
        {
            using call_type = route_call<P, decltype(entry.handler.handler)>;

            return std::apply(
                // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): they are, into the chain.
                [&entry]<typename... Layers>(Layers&&... items)
                {
                    return chained_handler<std::decay_t<Layers>..., call_type>{
                        std::in_place,
                        std::forward<Layers>(items)...,
                        call_type{std::move(entry.handler.handler)}};
                },
                std::move(entry.handler.layers));
        }
        else
        {
            return route_call<P, H>{std::move(entry.handler)};
        }
    }

    template<typename Entry>
    using route_type = decltype(make_route(std::declval<Entry>()));

    // allowed_methods lists the methods n handles, for an Allow header.
    std::string allowed_methods(const node& n);

    coro::task<void> respond(response_writer& resp, status status_code);

}

template<typename... Entries>
class static_router
{
    static constexpr std::array<detail::route_spec, sizeof...(Entries)> specs{
        detail::route_spec{.method = Entries::method, .pattern = Entries::pattern.view()}
        ...
    };

    static_assert(detail::check_routes(specs) != detail::routes_error::duplicate_route,
                  "two routes have the same method and pattern");
    static_assert(detail::check_routes(specs) != detail::routes_error::conflicting_parameter,
                  "two routes name a parameter differently, in the same place");

    static constexpr auto sizes = detail::table_size(specs);
    static constexpr auto table = detail::build_table<sizes.nodes, sizes.slots>(specs);

public:
    explicit constexpr static_router(Entries... entries)
        : routes{detail::make_route(std::move(entries))...}
    {}

    // operator() routes the request: it calls the handler for the request's method and path, or responds 404 Not
    // Found, or 405 Method Not Allowed.
    coro::task<void> operator()(server_request& req, response_writer& resp) const
    {
        const auto found = detail::match(table.nodes, table.slots, req);
        if (found.found == nullptr) return detail::respond(resp, status::NOT_FOUND);

        req.route = found.found->pattern;

        if (found.route == detail::none)
        {
            resp.headers().set(std::string_view{"Allow"}, detail::allowed_methods(*found.found));
            return detail::respond(resp, status::METHOD_NOT_ALLOWED);
        }

        return handlers[found.route](*this, req, resp);
    }

private:
    // call calls route I: its handler, or the chain of its middleware around it.
    template<std::size_t I>
    static coro::task<void> call(const static_router& self, server_request& req, response_writer& resp)
    {
        return std::get<I>(self.routes)(req, resp);
    }

    using handler_call = coro::task<void> (*)(const static_router&, server_request&, response_writer&);

    // the jump table: the route's handler, by its index.
    static constexpr auto handlers = []<std::size_t... I>(std::index_sequence<I...>)
    { return std::array<handler_call, sizeof...(I)>{&call<I>...}; }(std::index_sequence_for<Entries...>{});

    std::tuple<detail::route_type<Entries>...> routes;
};

// make_static_router makes a static_router of routes (and groups of them). Its first argument may be use(...):
// middleware for every route (but not the router's 404s and 405s).
template<typename... Args>
constexpr auto make_static_router(Args&&... args)
{
    return std::apply([](auto&&... entries)
                      { return static_router<std::decay_t<decltype(entries)>...>{std::move(entries)...}; },
                      detail::flatten_group(std::forward<Args>(args)...));
}

}
