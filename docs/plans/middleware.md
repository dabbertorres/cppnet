# Plan: a standard shape for HTTP server middleware

Status: planned (2026-10-18). The shape (option B below), middleware for some routes only (in the plan), and the
questions at the end were decided. Stage 1 (measuring) is done: B kept, with the costs corrected (Costs).

## Goal

One way to write middleware: code that runs around a request's handler (measuring it, tracing it, checking its
credentials, giving it a deadline), whether it wraps everything a server serves or only some of its routes. It should
cost as little per request beyond what the middleware itself does as the compiler allows (no allocation or
`std::function` per layer: Costs has what's left), as `bench_request` measures the rest of a request. The existing middleware (`instrument::http_metrics` and
`http_tracing`) are rewritten to it; the timeout middleware (stage 5 of `handler-cancellation.md`) is the first new
one.

## Where things stand

- `http::request_handler` is a `std::function<coro::task<void>(server_request&, response_writer&)>`
  (`http/router.hpp`): what the server calls, and what the runtime router's routes hold.
- Middleware is a convention of the two in `instrument/middleware.hpp`: a class constructed with the handler it wraps
  (`request_handler next`, or `router&&`) and options, a `const` `operator()(server_request&, response_writer&)`, and
  its state behind a `shared_ptr` (as `std::function` copies it). They compose by nesting constructors. Nothing names
  the convention, and nothing applies middleware to some routes only.
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

A concept, `http::middleware<M, Next>`, names it. A middleware is `const`-callable and movable (copyable or not: Decided
questions (2)); `next` is called at most once (3). It is configured independently of what it wraps, so one value can be
used on several routers or groups.

### Composing: `http::chain`

`http::chain(a, b, handler)` returns one value (a type holding `a`, `b` and `handler`, by value) that's itself a
handler (`operator()(req, resp)`): `a` runs outermost, then `b`, then `handler`. Each layer's `next` is a small object
of the chain's that calls the following layer directly, so `co_await next(req, resp)` is a direct call of a coroutine
in a `co_await` once the `next` object's call is inlined, which Clang's `coro-elide` elides into the calling layer's
frame (measured: Costs). The `next` objects aren't coroutines themselves (that would double the frames where nothing's
elided, as on GCC): empty bases of the chain (CRTP), reaching the following layer with `std::get`, so the chain stays
copyable and nothing in it points into itself. The chain allocates one frame for its outermost layer (the server calls
it through `request_handler`, an indirect call), with the inner layers elided into it; a handler the innermost layer
calls directly is elided too, but a router never is (below), so a chain around a router costs one allocation per
request more than the router alone, however many layers it has.

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
  `next_handler`) runs around the matched handler, outer subrouters' first, applying to the whole subrouter whatever
  order its routes and `use()` were added in (Decided questions (1)).

### The timeout middleware (from `handler-cancellation.md`, stage 5)

`http::timeout{duration}`: makes a `std::stop_source` linked to `req.stop` (a `std::stop_callback` that stops it; the
"helper for linking sources" of that plan, `util` or `coro`), stopped at the deadline, swaps it into `req.stop` for the
handler, and puts the original back. What stops it at the deadline is the cost: a timer per request. Options for the
stage itself: a scheduler `sleep_until(deadline, stop)` task per request (an allocation, and a wait registered), or
the scheduler's timers taking a callback rather than a coroutine (no task, but a new kind of timer). Measured before
it's chosen, as its own stage. What a timed-out handler's response is (503, as Go's `TimeoutHandler`, if nothing was
sent) is decided then too.

## Costs

The point of B over A is the allocations. Stage 1 measured them (2026-10-18; Clang 23.1.3, `-O3`, its
`-Rpass=coro-elide` remarks, and `bench_request` in Release with 2 workers, 7 rounds of each configuration, median;
throwaway layers that do nothing but `co_await next`; the prototype is not in the repo):

| Configuration | Socket test | Pipelined, in memory | Allocations per request (pipelined) |
|---|---|---|---|
| No middleware (the router, through a `request_handler`) | 27.4 us | 1116 ns | 1.4 |
| A, 1 layer / 3 layers | 27.6 / 27.8 us | 1196 / 1333 ns | 2.4 / 4.4 |
| B around the router, 1 layer / 3 layers | 28.2 / 27.8 us | 1197 / 1283 ns | 2.4 / 2.4 |
| A, 3 layers, calling the handler directly | 27.7 us | 1309 ns | 4.4 |
| B, 3 layers, calling the handler directly | 27.7 us | 1146 ns | 1.4 |
| The handler alone, no router | 27.3 us | 1090 ns | 1.4 |

- **B's inner layers are elided**, and so is a handler the innermost layer calls directly, and a layer that returns
  `next`'s task without a coroutine of its own costs no frame; the hooks' shape (opaque calls before and after, a
  `try`/`catch`) keeps elision.
- **A router is never elided into the layer that calls it:** `static_router` dispatches through a table of function
  pointers, and even a `switch` between direct calls isn't elided (Clang doesn't elide a task chosen among several
  coroutines). So `chain(layers..., router)` costs one allocation per request, flat, where A costs one per layer; the
  allocation-free way is middleware on routes (stage 3's `use()`), whose innermost layer calls the handler. Frame
  pooling, not the router's dispatch, is what would take the last one.
- **An elided layer isn't free:** in memory, about 18 ns per layer in front of a handler called directly, about 50 ns in
  front of the router (why the two differ wasn't found). On a socket, every configuration is within noise: the round
  trip through the kernel hides them.
- **GCC elides nothing**, so there B costs a frame per layer, as A does, saving only the `std::function` call.

## Testing

- `chain`: order (outermost first), a layer answering without calling `next`, a layer returning `next`'s task without
  a coroutine, exceptions passing through, `req.stop` swapped and restored.
- The instrumentation's existing tests, through `chain`.
- Groups: `static_router` middleware runs only for its group's routes (not 404s, other groups, or the root), in order
  with nested groups', with `params` reaching the handler; the runtime router's the same, through subrouters.
- The timeout: a handler waiting past it is stopped (its token, and a wait it gave the token), the original token is
  restored, a client's stop still reaches the handler through the linked source.

## Stages

1. **Measure:** the benchmark above, with throwaway middleware of both shapes. Report before going on. Done as above
   (Costs): B kept, on the user's decision, with the claims corrected (a router isn't elided).
2. **The shape:** the concept, `chain`, `next_handler`, the tests; the instrumentation rewritten (hooks), the example,
   the guide.
3. **Some routes only:** `static_router`'s `use()` in groups, the runtime router's `use()` on subrouters.
4. **The timeout middleware**, with the source-linking helper (measuring its timer first): `handler-cancellation.md`'s
   stage 5 points here.

## Decided questions

1. **When the runtime router's `use()` applies:** to every route of the subrouter, and its subrouters, whatever the
   order they were added in: the tree decides, as `static_router`'s groups do. (Not Echo's and chi's order, where it
   applies only to routes added after it, and chi panics if `Use` follows a route.)
2. **Move-only middleware is allowed:** `request_handler` is a `std::function`, which needs copies, so a chain that
   isn't copyable is wrapped as `make_handler` wraps what isn't (a `shared_ptr`).
3. **`next` is called at most once** (a request's body is read once, and its response sent once): documented, not
   enforced.
4. **Names:** `http::chain`, `use`, `http::next_handler`, and the `http::middleware` concept.

## Out of scope

- Middleware for the HTTP client (Go's `RoundTripper` wrappers): a similar shape around `client::send`, planned
  separately if wanted.
- Middleware chosen at runtime by configuration (a list read from a file): a chain's type is fixed at compile time;
  such a list would be a type-erased layer of its own.
