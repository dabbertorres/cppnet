# Plan: a standard shape for HTTP server middleware

Status: planned (2026-10-18). The shape (option B below) and middleware for some routes only (in the plan) were
decided; the open questions at the end are still to be decided.

## Goal

One way to write middleware: code that runs around a request's handler (measuring it, tracing it, checking its
credentials, giving it a deadline), whether it wraps everything a server serves or only some of its routes. It should
cost nothing per request beyond what the middleware itself does (no allocation, no `std::function` per layer), as
`bench_request` measures the rest of a request. The existing middleware (`instrument::http_metrics` and
`http_tracing`) are rewritten to it; the timeout middleware (stage 5 of `handler-cancellation.md`) is the first new
one.

## Where things stand

- `http::request_handler` is a `std::function<coro::task<void>(server_request&, response_writer&)>`
  (`http/router.hpp`): what the server calls, and what the runtime router's routes hold.
- Middleware is a convention of the two in `instrument/middleware.hpp`: a class constructed with the handler it wraps
  (`request_handler next`, or `router&&`) and options, a `const` `operator()(server_request&, response_writer&)`, and its
  state behind a `shared_ptr` (as `std::function` copies it). They compose by nesting constructors. Nothing names the
  convention, and nothing applies middleware to some routes only.
- Each layer calls the next through a `std::function`: an indirect call, so Clang never elides the next coroutine's
  frame into its own (CLAUDE.md's HALO notes): a heap allocation per layer per request.
- Middleware passes values to handlers in `req.extensions` (a `util::type_map`), and may swap `req.stop` for the
  handler it wraps (`handler-cancellation.md`'s "Middleware and routers").

## Decided

- **Option B: `next` passed at call time, composed at compile time.** Rejected: A, formalizing the current convention
  (Go's `func(Handler) Handler`; a `std::function` call and an allocation per layer), and C, hooks before and after the
  handler (Crow's; they can't wrap the `co_await`, which a timeout, error handling and swapping the token all need).
- **Middleware for some routes only** is in the plan (a stage after the shape): `static_router`'s groups and the runtime
  router's subrouters.

## Design

### The shape

A middleware is a value (its options, and whatever state it shares), with:

```cpp
template<class Next>
coro::task<void> operator()(server_request& req, response_writer& resp, Next& next) const;
```

`next(req, resp)` runs the rest (more middleware, then the handler) and returns its task. A middleware may:

- `co_await next(req, resp)` with work around it (the usual case);
- not call it, answering itself (e.g. 401);
- return `next(req, resp)` without a coroutine of its own, if it only works before (e.g. sets an extension): no frame.

A concept, `http::middleware<M, Next>`, names it. A middleware is `const`-callable and copyable (or movable: see the
open questions), and is configured independently of what it wraps, so one value can be used on several routers or
groups.

### Composing: `http::chain`

`http::chain(a, b, handler)` returns one value (a type holding `a`, `b` and `handler`, by value) that's itself a
handler (`operator()(req, resp)`): `a` runs outermost, then `b`, then `handler`. Each layer's `next` is a small object
of the chain's that calls the following layer directly, so `co_await next(req, resp)` is a direct call of a coroutine
in a `co_await`, which Clang's `coro-elide` can elide into the calling layer's frame: the chain should allocate one
frame for its outermost layer (the server calls it through `request_handler`, an indirect call, as it does a router
today), with the inner layers and the router elided into it. **That's the expectation to measure first** (Costs).

The server's constructor already takes a `request_handler`; a chain converts to one (`make_handler` if it isn't
copyable). No change to the server.

### Middleware whose work belongs in a `.cpp`

A template's body is in a header. Middleware with heavy internals (the instrumentation's: glaze, the registry) keeps
them out of line, two ways:

1. **Hooks (recommended for the instrumentation):** a few lines of coroutine in the header, calling functions in the
   `.cpp` before and after:
   ```cpp
   auto scope = state->begin(req);
   co_await next(req, resp);   // exceptions: caught, then end() and rethrow, as today's
   state->end(scope, req, resp);
   ```
   The layer stays elidable.
2. **`http::next_handler`:** a non-owning reference to the rest (two pointers, as `std::function_ref`), which a
   non-template `operator()(req, resp, next_handler next)` takes, so the whole middleware is in the `.cpp`. Calling
   it is indirect, so the rest isn't elided: an allocation per request, as today. For middleware where that doesn't
   matter.

### The existing instrumentation

`http_metrics` and `http_tracing` become middleware values: constructed with their options alone (no `next`, no
`router&&` overload), their state (metrics, the span callback) still shared, by `shared_ptr`, as copies of a metric
must update the same series. `operator()` is the hooks' shape. Uses change from `http_metrics{std::move(router)}` to
`chain(http_metrics{}, std::move(router))`: the example, the tests, the guide.

### Some routes only

Middleware applied to some routes runs after routing, so it sees `req.route` and the path values; middleware wrapping
the router (through `chain`) runs for every request, including 404s and 405s.

- **`static_router`:** `group("/api"_route, use(auth{}, limits{}), GET(...), ...)`: `use(...)` in a group composes its
  middleware with each of the group's routes' handlers at compile time (a chain whose innermost layer calls the
  handler with its `params`). Nested groups' middleware runs outermost first.
- **The runtime router:** `subrouter("/api", [](router& r) { r.use(auth{}); r.GET(...); })`. Its handlers are
  `std::function`s already, so the middleware is too: a node's middleware (type-erased, each called with a
  `next_handler`) runs around the matched handler, outer subrouters' first. When `use()` applies (open question 1)
  decides how it's stored.

### The timeout middleware (from `handler-cancellation.md`, stage 5)

`http::timeout{duration}`: makes a `std::stop_source` linked to `req.stop` (a `std::stop_callback` that stops it; the
"helper for linking sources" of that plan, `util` or `coro`), stopped at the deadline, swaps it into `req.stop` for the
handler, and puts the original back. What stops it at the deadline is the cost: a timer per request. Options for the
stage itself: a scheduler `sleep_until(deadline, stop)` task per request (an allocation, and a wait registered), or
the scheduler's timers taking a callback rather than a coroutine (no task, but a new kind of timer). Measured before
it's chosen, as its own stage. What a timed-out handler's response is (503, as Go's `TimeoutHandler`, if nothing was
sent) is decided then too.

## Costs

The point of B over A is the allocations. Before the shape is built on, measure with `bench_request` (a Release build,
alternating runs, as stage 4 of the cancellation plan did): the socket test with no middleware, one layer and three
layers (layers that do nothing but `co_await next`), as option A (today's: `std::function` per layer) and as B. B
should add no allocations per request and be within noise of no middleware. If Clang doesn't elide the inner layers
(it elides only calls not in loops; GCC doesn't elide at all, so it's Clang's builds that are measured), B's case
rests on the rest (no `shared_ptr`, no `std::function`, reuse), and that goes back to the user before going on.

## Testing

- `chain`: order (outermost first), a layer answering without calling `next`, a layer returning `next`'s task without
  a coroutine, exceptions passing through, `req.stop` swapped and restored.
- The instrumentation's existing tests, through `chain`.
- Groups: `static_router` middleware runs only for its group's routes (not 404s, other groups, or the root), in order
  with nested groups', with `params` reaching the handler; the runtime router's the same, through subrouters.
- The timeout: a handler waiting past it is stopped (its token, and a wait it gave the token), the original token is
  restored, a client's stop still reaches the handler through the linked source.

## Stages

1. **Measure:** the benchmark above, with throwaway middleware of both shapes. Report before going on.
2. **The shape:** the concept, `chain`, `next_handler`, the tests; the instrumentation rewritten (hooks), the example,
   the guide.
3. **Some routes only:** `static_router`'s `use()` in groups, the runtime router's `use()` on subrouters.
4. **The timeout middleware**, with the source-linking helper (measuring its timer first): `handler-cancellation.md`'s
   stage 5 points here.

## Open questions

1. **When the runtime router's `use()` applies:** (a) to every route of the subrouter, and its subrouters, whatever
   the order they were added in (recommended: the tree decides, as `static_router`'s groups do); or (b) only to
   routes added after it (Echo's and chi's order; chi panics if `Use` follows a route).
2. **Copyable or movable-only middleware:** `request_handler` is a `std::function`, which needs copies; `make_handler`
   already wraps what isn't copyable in a `shared_ptr`. Recommended: allow move-only, and wrap as `make_handler` does.
3. **`next` called more than once** (e.g. a retry): recommended, documented as at most once (a request's body is read
   once, and its response sent once), not enforced.
4. **Names:** `chain`, `use`, `next_handler`, the `middleware` concept.

## Out of scope

- Middleware for the HTTP client (Go's `RoundTripper` wrappers): a similar shape around `client::send`, planned
  separately if wanted.
- Middleware chosen at runtime by configuration (a list read from a file): a chain's type is fixed at compile time;
  such a list would be a type-erased layer of its own.
