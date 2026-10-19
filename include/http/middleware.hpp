#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"

// HTTP server middleware: code that runs around a request's handler (measuring it, tracing it, checking its
// credentials), composed with the handler at compile time by chain():
//
//   http::server server{&scheduler, http::chain(instrument::http_metrics{}, auth{...}, std::move(router))};
//
// A middleware is a value (its options, and whatever state its copies share) with a const operator() that takes the
// request, its response_writer and next, the rest of the chain (more middleware, then the handler):
//
//   struct auth
//   {
//       template<class Next>
//       coro::task<void> operator()(http::server_request& req, http::response_writer& resp, Next& next) const
//       {
//           if (!allowed(req))
//           {
//               co_await resp.send(http::status::UNAUTHORIZED, 0);
//               co_return;
//           }
//
//           co_await next(req, resp);
//       }
//   };
//
// next(req, resp) runs the rest of the chain, and returns its task. A middleware may:
// - co_await next(req, resp), with work around it (the usual case);
// - not call it, answering the request itself (e.g. 401 Unauthorized);
// - return next(req, resp), without being a coroutine itself, if its work is all before the rest (e.g. putting a value
//   in req.extensions): then it costs no coroutine frame.
//
// next is called at most once (a request's body is read once, and its response sent once): that isn't checked.
//
// The layers run outermost first: chain(a, b, h) runs a, whose next runs b, whose next runs h. A middleware is
// configured independently of what it wraps, so one value can be used in several chains. A middleware may swap
// req.stop for the rest of the chain (putting the request's back after), and pass values to it in req.extensions. A
// chain must outlive the requests it handles (a server keeps its own).
//
// A middleware whose operator() is a template on Next (taken as Next&) is called with the chain's own next objects,
// whose calls are direct: Clang elides the coroutine frames of the inner layers (and of a handler the innermost layer
// calls, though not of a router's handlers) into the outermost one's. One whose body belongs out of line (in a .cpp)
// takes a next_handler instead, a reference to the rest of the chain that's called indirectly (so the rest's frame
// isn't elided); or keeps a few lines of coroutine in its header, a template on Next, that call out-of-line functions
// before and after next (as instrument::http_metrics does).
namespace net::http
{

// next_handler is a reference to the rest of a chain (see chain()), as a middleware whose operator() isn't a template
// takes it:
//
//   coro::task<void> operator()(http::server_request& req, http::response_writer& resp, http::next_handler next) const;
//
// It's two pointers (as std::function_ref): the object it refers to, and a function that calls it. It refers to its
// object without owning it, so it can't be made from a temporary; the chain's next objects outlive the request.
class next_handler
{
public:
    // next_handler refers to h, which must outlive it.
    template<class H>
        requires(!std::same_as<std::remove_cv_t<H>, next_handler>)
                 && std::is_invocable_r_v<coro::task<void>, const H&, server_request&, response_writer&>
    // NOLINTNEXTLINE(*-explicit-constructor): the chain's next objects convert, as a middleware's parameter.
    next_handler(const H& h) noexcept
        : object{std::addressof(h)}
        , call{[](const void* referred, server_request& req, response_writer& resp) -> coro::task<void>
               { return std::invoke(*static_cast<const H*>(referred), req, resp); }}
    {}

    template<class H>
    next_handler(const H&&) = delete;

    // operator() runs the rest of the chain.
    coro::task<void> operator()(server_request& req, response_writer& resp) const { return call(object, req, resp); }

private:
    const void* object;
    coro::task<void> (*call)(const void*, server_request&, response_writer&);
};

// middleware is what chain() composes: a value that's called with the request, its response, and next, the rest of the
// chain (see the top of this header). Next is the chain's own next object (a template operator() is called with it),
// or next_handler.
template<class M, class Next = next_handler>
concept middleware = std::is_object_v<M> && std::move_constructible<M>
                  && requires(const M& m, server_request& req, response_writer& resp, Next& next) {
                         { m(req, resp, next) } -> std::same_as<coro::task<void>>;
                     };

// handler is what a chain ends with: something called with the request and its response (e.g. a router, or a
// function), as a request_handler is (see http/router.hpp), but through a const reference.
template<class H>
concept handler = std::is_object_v<H> && std::move_constructible<H>
               && std::is_invocable_r_v<coro::task<void>, const H&, server_request&, response_writer&>;

namespace detail
{

    template<class Chain, class Indexes>
    struct chain_nexts;

    // chain_next is a chain's next object for its item I: an empty base of the chain (so the chain stays copyable, and
    // nothing in it points into itself), whose call runs the chain from item I. It isn't a coroutine, so a layer that
    // awaits it awaits item I's coroutine directly, which Clang can elide.
    template<class Chain, std::size_t I>
    struct chain_next
    {
        coro::task<void> operator()(server_request& req, response_writer& resp) const
        {
            return static_cast<const Chain&>(*this).template run<I>(req, resp);
        }

    private:
        // only the chain makes them (through chain_nexts).
        chain_next() = default;

        friend Chain;

        template<class, class>
        friend struct chain_nexts;
    };

    // chain_nexts derives from the chain's next objects for its items 1 to the last.
    template<class Chain, std::size_t... I>
    struct chain_nexts<Chain, std::index_sequence<I...>> : chain_next<Chain, I + 1>...
    {
    private:
        // only the chain makes them.
        chain_nexts() = default;

        friend Chain;
    };

    // is_valid_chain says whether Items are middleware, then a handler.
    template<class... Items>
    consteval bool is_valid_chain()
    {
        using items           = std::tuple<Items...>;
        constexpr auto layers = sizeof...(Items) - 1;

        return []<std::size_t... I>(std::index_sequence<I...>)
        { return (middleware<std::tuple_element_t<I, items>> && ...); }(std::make_index_sequence<layers>{})
            && handler<std::tuple_element_t<layers, items>>;
    }

    template<class... Items>
    inline constexpr bool valid_chain = is_valid_chain<Items...>();

}

// chained_handler is the handler chain() makes: its items by value, middleware then a handler. It's copyable if they
// all are, and otherwise move-only (a server takes either: see http::server).
template<class... Items>
class chained_handler
    : private detail::chain_nexts<chained_handler<Items...>, std::make_index_sequence<sizeof...(Items) - 1>>
{
public:
    template<class... Args>
    explicit chained_handler(std::in_place_t /*tag*/, Args&&... args)
        : items{std::forward<Args>(args)...}
    {}

    // operator() runs the chain: its outermost middleware, or its handler if it has none.
    coro::task<void> operator()(server_request& req, response_writer& resp) const { return run<0>(req, resp); }

private:
    template<class, std::size_t>
    friend struct detail::chain_next;

    static constexpr std::size_t last = sizeof...(Items) - 1;

    template<std::size_t I>
    coro::task<void> run(server_request& req, response_writer& resp) const
    {
        if constexpr (I == last)
        {
            return std::invoke(std::get<I>(items), req, resp);
        }
        else
        {
            const auto& next = static_cast<const detail::chain_next<chained_handler, I + 1>&>(*this);
            return std::get<I>(items)(req, resp, next);
        }
    }

    std::tuple<Items...> items;
};

// chain composes middleware with a handler (the last argument), into one handler: the first middleware runs outermost,
// and the handler innermost (see the top of this header). It holds copies of them (or what's moved in: e.g. a router,
// which makes the chain move-only).
template<class... Items>
    requires(sizeof...(Items) >= 1) && detail::valid_chain<std::decay_t<Items>...>
chained_handler<std::decay_t<Items>...> chain(Items&&... items)
{
    return chained_handler<std::decay_t<Items>...>{std::in_place, std::forward<Items>(items)...};
}

}
