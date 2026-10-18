# Plan: canceling HTTP server handlers

Status: stage 1 (waits that take a token) done (2026-10-18); stages 2 to 5 planned. Where things stand describes what
was there before (HTTP/1.1's graceful `close()`, and the `aborting` source the destructor stops), and stage 1's
section, what it added. The questions below (under Decided questions) were decided as recommended.

## Goal

A handler learns when its work is no longer wanted, so it can stop, and give back what it holds (its HTTP/2 stream's
place among `http2_max_concurrent_streams`, and its connection's among `max_connections`), rather than running to the
end for no one. That's: its HTTP/2 stream reset by the client, timed out, or its connection ended; its HTTP/1.1
connection closed; or the server stopping. Canceling is cooperative: the handler sees a stop request, and the waits it
gives the request to end early with `std::errc::operation_canceled`; nothing is destroyed from under it. Go's
`r.Context()` is the model, as far as C++ allows.

## Decided

- **`std::stop_token`**, not a mechanism of the library's own. It's what C++ code already takes (`std::jthread`,
  `std::condition_variable_any::wait`, and here `tls::stream::connect()`/`accept()` and the server's own `closing`), so
  a handler can hand it to code that knows nothing of cppnet. Its cost is an allocation per `std::stop_source` (both
  libc++ 23 and libstdc++ `new` its shared state in the default constructor) and a few atomic operations per callback
  registered; see Costs for where that lands. An in-house flag would avoid the allocation but interoperate with
  nothing; C++26's `std::inplace_stop_source` (P2300: no allocation, the state lives in the source) is the natural
  replacement, but neither libc++ 23 nor libstdc++ 14 (the local GCC) has it.
- **Explicit, not implicit.** The token travels as a value (on `server_request`, and handed to waits and requests),
  as Go's `ctx` does, not through the coroutines' promises as C++26's `std::execution::task` does with its environment
  (P3552): that would put another pointer in every promise and a copy in every `co_await`'s hand-over (the path
  `bench_task` measures), and make spawned tasks' inheritance a question. See Decided questions (8).
- **The source per HTTP/1.1 connection, per HTTP/2 stream.** An HTTP/1.1 request can only be canceled by its
  connection ending (there's no way to cancel one request and keep the connection), so one `std::stop_source` per
  connection, made when its first request is read, serves all of its requests: nothing per request. An HTTP/2 stream
  can be reset alone, so each stream has its own (with the server's `http2_stream`, which is allocated per stream
  already).
- **What stops a handler's token.** HTTP/2: the client resetting the stream (any code, NO_ERROR included: nothing more
  the handler sends reaches the client), the stream timing out (`check_streams`), the stream failing to send
  (`stream_writer`'s submit failing), and the connection ending (`http2_connection_base::end()`: a connection error,
  ENHANCE_YOUR_CALM, a failed write, the client closing, the idle or preface timeouts). Both versions: the server's
  hard stop (see Decided questions (2): the destructor, `scheduler::request_stop()`, and an explicit call), and for
  HTTP/1.1, the client leaving while the handler runs, once stage 4 notices it. The peer's GOAWAY doesn't (streams up
  to its last stream go on), nor does graceful `close()` (Decided questions (2)).
- **Stop requests never run under the connection's lock.** `std::stop_source::request_stop()` runs the callbacks
  registered on it inline, and those are anyone's (a handler's, a scheduler wait's, a client request's, taking their
  own locks); HTTP/2's session events, `check_streams` and `end()` all run holding the connection's `mu`. So where a
  stream fails, its source is copied (a reference count, no allocation) onto a list guarded by `mu`, and the list is
  stopped once `mu` is released: at the points that already act after unlocking (`receive()`'s `after_receive()`, the
  timer loop after its check, the writer and `run()` after `end()`). A copy, rather than a pointer to the stream,
  because the handler may finish and its stream be freed in between. The same holds for the server's hard stop: it
  takes `mu` to collect a connection's sources, and stops them after.
- **`max_connections` is unchanged.** A connection's slot is still held until its handlers return (review-fixes group
  5's bound): canceling makes that short for handlers that heed it, and changes nothing for those that don't.
- **What canceled looks like.**
  - To the handler: `req.stop.stop_requested()`; a wait given the token fails with `std::errc::operation_canceled`
    (as `request_stop()`'s do); an HTTP/2 body read or write fails with the stream's failure, as now
    (`connection_reset` for a reset, `timed_out`, `connection_aborted` for the connection ending). The reason isn't
    otherwise exposed (Decided questions (4)).
  - To the client: nothing new for HTTP/2 (its reset, or the server's RST_STREAM(CANCEL) for a timeout, or the
    connection's end, already happened); for the server's hard stop, the handler may still answer (e.g. a 503), and a
    response it leaves incomplete is reset with CANCEL rather than INTERNAL_ERROR. For HTTP/1.1, the connection's
    closed: after the response, if the handler sends one and the connection can still take it.
  - In the logs: a handler that throws once its token is stopped (e.g. a database driver's own "canceled") is logged
    at debug, not error, and gets no 500 if its client is gone.
- **The server holds the token it gives.** It sets `req.stop` before each handler call (HTTP/1.1 reuses its request,
  and middleware may have replaced the field), and `server_request::clear()` leaves it alone, as it does `tls`.
- **`run_blocking` isn't interrupted.** Resuming the awaiting coroutine while `func` still runs would need `func` (and
  what it refers to) to outlive the call. Instead `run_blocking(func, stop)` doesn't start `func` if stop's already
  requested, and passes the token to a `func` that takes one (`std::invocable<F, std::stop_token>`), for blocking code
  that can check it. `getaddrinfo()` can't, so a connect's resolution finishes before the request sees it was
  canceled (as the DNS plan says of the system backend).
- **`coro::notifier` doesn't take tokens.** Its waiters are HTTP/2's streams, connection writers and TLS's flush
  queue, which their owners wake by closing them or failing the stream; a canceled client request resets its stream,
  which wakes its waits the way a reset by the server does.

## Specifications and references

- C++20's stop tokens ([thread.stoptoken]): `std::stop_source`, `std::stop_token`, `std::stop_callback` (whose
  callback runs inline in `request_stop()`'s caller if registered before, or in its constructor if after; whose
  destructor waits for a callback running on another thread).
- [P2300](https://wg21.link/p2300) (`std::execution`, C++26): `std::inplace_stop_source`/`inplace_stop_token`, the
  `stoppable_token` concept; [P3552](https://wg21.link/p3552) (`std::execution::task`), whose environment carries a
  stop token implicitly (`co_await read_env(get_stop_token)`).
- Go's `net/http`: `Request.Context()` "is canceled when the client's connection closes, the request is canceled (with
  HTTP/2), or when the ServeHTTP method returns". For HTTP/1.1 it notices the client leaving with a background read
  (`connReader.startBackgroundRead`), started once the request's body has been read to its end (or at once, without
  one): EOF or an error cancels the context, and a byte that arrives (a pipelined request) is kept for the next.
  `Server.Shutdown` drains without canceling contexts; `Server.Close` closes the connections (which cancels them);
  `Server.BaseContext` ties every request's context to one of the application's; `http.TimeoutHandler` bounds a
  handler.
- hyper (Rust) cancels by dropping: a request's future is dropped with its connection, or its HTTP/2 stream's reset.
- nginx logs 499 ("Client Closed Request") for a request whose client went away first.
- RFC 9113, section 8.1 and 6.4 (RST_STREAM, NO_ERROR's "stop sending"), and 5.4.1 (closing after a connection
  error).

## Where things stand

- `server_request` has no token. `request_handler` is `std::function<coro::task<void>(server_request&,
  response_writer&)>`.
- `server` has a `std::stop_source closing`, which `close()` stops, gracefully for both protocols. HTTP/1.1
  connections register a callback that calls `shutdown_read()` on a connection waiting for a request (its first, or
  the next), so it ends at once; one in the middle of a request (its head read) finishes it, its response saying
  `Connection: close` if its head wasn't sent yet (`response_writer` checks `closing`'s token), and then closes,
  without serving requests pipelined after it. An atomic phase per connection (idle, busy, closing), which the callback
  and the connection each move, decides which, so a head arriving as `close()` runs is either served whole or not at
  all. HTTP/2 connections register one that begins a graceful drain (GOAWAY, PING, final GOAWAY). `server` also has
  `std::stop_source aborting`, which `~server` stops after `close()`: HTTP/1.1 connections register a callback on it
  that calls `shutdown_read()` whatever they're doing, so a body being read fails, with `connection_aborted` (a body
  cut short is that, framed by Content-Length as well as chunked: `limit_reader` reports its reader ending before its
  limit as `connection_aborted`, rather than passing the end on). HTTP/2 connections don't register on `aborting`
  yet: `~server` waits for their streams. Nothing reaches a handler that isn't reading its body.
- HTTP/2's streams (`http2_stream_base`) have a `failure` code, set by the client's reset (`on_stream_reset`), a
  timeout (`check_streams`, which only arms a deadline while the handler waits to read or write the stream: a handler
  doing neither never times out), a failed submit, and `end()`; each wakes the stream's `notifier`. Each stream is
  allocated (`std::make_unique<http2_stream>`, and its `stream_reader`), with its handler's task and the scope's
  frame.
- `scheduler::request_stop()` cancels every wait, all at once, and a wait can't be canceled alone (except by
  deregistering its handle, or `handle_operation::cancel_wait()`, which doesn't resume the coroutine: it's for one
  being destroyed). `wait_operation` holds its `waiter` in the awaiting coroutine's frame.
- `tls::stream::connect()`/`accept()` take a `std::stop_token`, whose callback shuts the socket's reading side: the
  one API that takes a token.
- `http::client::send()` has no way to be canceled; its waits are the pool's (`wait_on` for a connection, a stream's
  `notifier` for an HTTP/2 response's head), sockets' (connecting, reading, writing), and `run_blocking` (resolving).
- `instrument::http_metrics` and `http_tracing` record the status sent (200 if the handler sent nothing, 500 if it
  threw); a canceled request is recorded as whatever the handler did.
- `bench_request` measures HTTP/1.1 only (decoding, handling and the rest, in memory, and over a socket); nothing
  counts HTTP/2's allocations per stream.

## Design

### The request's token

`server_request` gets a public field, `std::stop_token stop` (Decided questions (1)), documented with what stops it and
how to use it: check `stop.stop_requested()` between steps of a long computation; register a `std::stop_callback`
(quick, non-blocking, as it may run on the connection's reader) to interrupt something else; or pass it to the waits
that take one (below), and to `client_request::stop` for outgoing requests. A token copied out of the request stays
valid after the handler returns (it holds the shared state).

- **HTTP/1.1.** `serve_requests` makes a `std::stop_source` once it knows the connection is HTTP/1.x (after the
  preface check), with a `std::stop_callback` on the server's hard-stop token that stops it, and sets `req.stop` from
  it before each handler. It's stopped when the connection ends (the loop exits: nothing's running then, but a token
  a handler kept, e.g. in a task it spawned, sees it), by the hard stop, and (stage 4) when the client leaves during a
  handler.
- **HTTP/2.** `http2_stream` gets a `std::stop_source` (made with the stream in `start_stream`), and `req.stop` its
  token. The base's failure sites (`on_stream_reset`, `check_streams`, `end()`, `stream_writer`'s failed submit) call
  a new virtual, `stop_source_of(http2_stream_base&)` (holding `mu`; the server's connection returns its stream's
  source, the client's none), and copy what it returns onto the base's `to_stop` (a vector guarded by `mu`, reserved
  so the rare path rarely allocates). The base's `stop_pending()` swaps it out under `mu` and stops each source after
  releasing it; it's called where the connection already acts after unlocking (after `receive()`, the timer loop's
  check, `end()`'s callers in the writer and `run()`, and a handler's own failed write). The server's connection
  registers a callback on the hard-stop token that collects its running streams' sources under `mu`, and stops them
  after.
- **A handler that returns after its token was stopped.** HTTP/2: if the stream failed, it's removed as now (no
  response, no reset: the client's reset or the server's was sent); if the token was stopped by the hard stop and the
  response is incomplete, it's reset with CANCEL. HTTP/1.1: the response, if any, is sent (with `Connection: close`,
  as the hard stop closes the server too, whose token `response_writer` checks) and the connection closed; if the
  client's gone, nothing is sent. An exception is logged at debug level, without a 500 where the client's gone.

### The server's stops

`server`'s second source, `aborting` (beside `closing`), becomes the hard stop: today only `~server` stops it, and only
HTTP/1.1 connections register on it (to shut their reading side). Which calls stop it is Decided questions (2):

- `close()` stays graceful: it stops accepting, ends idle connections, lets HTTP/1.1's requests in progress finish,
  and drains HTTP/2 (GOAWAY), but doesn't stop handlers' tokens.
- `abort()` (new) does what `close()` does, and stops every handler's token: for a program that drains for a while
  and then gives up, as Go's `Shutdown` then `Close`.
- `join()` (new, a task) waits until `serve()` and every connection have finished (the `running` scope's `join()`), so
  a program can `close()`, `co_await server.join()` (perhaps racing a deadline, then `abort()`), and only then shut the
  scheduler down. Today it can only give the drain time before `scheduler.shutdown()`, which cancels at once; the
  destructor waits the same way, but blocking, and must not run on one of the scheduler's tasks. (Decided 2026-10-18,
  after HTTP/1.1's `close()` became graceful.) `example/http-server` uses it, with a bound on how long it drains.
- `~server` aborts (it must wait for the handlers anyway, and nothing will read their answers).
- `scheduler::request_stop()` aborts, through a token the scheduler gets (`scheduler::stop_token()`, a
  `std::stop_source` it stops in `request_stop()`), which the server's constructor registers a callback on. It
  already cancels every wait; this reaches handlers between waits (computing, or waiting on something that isn't the
  scheduler's). `example/http-server`'s `close()` then `shutdown()` gets the hard stop without a change.

### Waits that take a token (`io/scheduler.hpp`, sockets)

What makes the token useful in a handler: a `co_await scheduler.sleep_until(deadline, req.stop)` that ends when the
client resets the stream.

- **Overloads, and their own awaiter.** `wait(fd, op, stop)`, `wait_until(fd, op, deadline, stop)`, `wait_for(...,
  stop)`, `wait_on(word, expected, deadline, stop)` and `sleep_until(deadline, stop)` return a
  `stoppable_wait_operation`: a `wait_operation` plus an `std::optional<std::stop_callback<canceler>>` (`canceler` is
  `{scheduler*, waiter*}`). The overloads without a token, and their awaiter, are unchanged, so coroutines that don't
  pass one don't get bigger frames or another branch.
- **The race.** The callback can run at any moment from another thread, including before the wait is registered, and
  `std::stop_callback`'s constructor runs it inline if stop was already requested. So `await_suspend` emplaces the
  callback *before* taking the shard's lock (or `timers_mu`), and the waiter gets a `canceled` flag: the callback takes
  the same lock, and if the waiter's registered, completes it with `operation_canceled` (as `request_stop()` does, then
  resumes it on the pool); if not yet, sets `canceled`, which `suspend_*` checks (under the lock) to fail without
  suspending. If it already completed, there's nothing to do. `await_resume` resets the callback, whose destructor
  waits for one still running on another thread (which, having completed the wait, only has the pool's resume left).
- **Sockets.** `io::reader`/`io::writer` have no room for a token in `read`/`write`, so a socket takes one as it takes
  deadlines: `socket::set_stop_token(std::stop_token)` (Decided questions (6)), which its `read_operation` and
  `write_operation` register a callback on while they wait (the callback in the operation, which is per socket, so
  nothing's allocated), completing the wait with `operation_canceled` under the shard's lock. A wait canceled before
  `perform()` read anything loses nothing (both run under that lock), so the socket stays usable. `tcp_socket::connect`
  takes one in its options. `tls::stream` gets `set_stop_token` too, forwarding to its socket. A canceled read isn't a
  stream failure (it records only alerts and protocol errors as failures, which stays so); a canceled write is, for
  writing (`write_queued` already records `operation_canceled` when abandoned, as part of a record may have gone), so
  a TLS connection whose write was canceled can't be written again: fine for the client, which closes it anyway.
- **`run_blocking(func, stop)`** as Decided.

Done as above, with these details settled in the code:

- `run_blocking(func, stop)` returns a task of `std::expected<R, std::error_code>` (R being `func`'s result), failing
  with `operation_canceled` when `func` didn't run.
- The token-taking `wait_until` takes an `std::optional<clock::time_point>` (there's no separate `time_point`
  overload); `wait_on`'s token comes after its deadline, which it then has no default for.
- `waiter`'s `canceled` flag sits in padding, so `wait_operation` (112 bytes) and the token-less paths' frames don't
  grow; their only addition is reading that flag (a plain `bool`, under the lock they already hold) beside
  `stopping`. The stop callback completes a registered wait under its lock, and resumes it on the pool after
  releasing it (one handle, no vector).
- `handle_operation` holds the callback (`std::optional<std::stop_callback>`), registered in `begin()` after its
  waiter is reset, and deregistered by `end_wait()` (which the socket's `result()` calls before releasing the
  operation) or `cancel_wait()`. A socket grows by its token and the two operations' callbacks (520 to 672 bytes).
- `connect`'s `stop` covers connecting only: the socket returned doesn't take it (the client sets its own in stage 3).
- `scheduler::stop_token()` is stopped at the end of `request_stop()`, after the waits are canceled and holding none
  of the scheduler's locks.
- `bench_request`: unchanged (time within noise, the same allocations per request); `bench_task` doesn't involve the
  scheduler (its binary is identical).

### Outgoing requests (`http::client`)

`client_request` gets `std::stop_token stop` (Decided questions (5)), and `client::get(url, stop)`. A request canceled:

- **while it waits for a connection or stream** (`connection_pool`'s `wait_on`): the stoppable `wait_on`.
- **while connecting**: the socket's token (`connect` options), and TLS's handshake (which takes one already).
  Resolution isn't interrupted (`run_blocking`), but its result is dropped.
- **over HTTP/1.1**: a callback (in `send()`'s frame, then moved with the connection into the response's body reader,
  which is already allocated) shuts the connection down both ways, so the pending read or write fails; the connection
  is closed rather than returned to the pool. The result is `operation_canceled`, not the I/O error it caused.
- **over HTTP/2**: the callback resets the stream (RST_STREAM(CANCEL)), under the client connection's `mu` (the
  callback runs on whoever stopped the token, never under that `mu`), which fails the stream and wakes its waits; the
  connection stays in the pool. Reading the body after `send()` returns is covered the same way.

So `out.stop = req.stop;` in a handler cancels its outgoing requests with it.

### HTTP/1.1: noticing the client leave (stage 4)

The server doesn't read an HTTP/1.1 connection while its handler runs, so a client that closes isn't seen until the
response is written (EPIPE) or the next request is read. Go reads in the background (see references). Here, Decided
questions (3); the recommended design:

- **A watcher per connection**, not per request (a task per request would be a frame, or two with `async_scope`'s, on
  `bench_request`'s path): started with the connection's first watched request, parked on a `notifier` between them.
- **When it watches**: once the request's body is at its end (at once for a request without one: most GETs), and only
  if nothing's buffered after it (a pipelined request already there means the client was, and its bytes stay in the
  buffer for the next decode).
- **How**: `state.reader.fill()`, so what arrives lands in the connection's own buffer, through `tls::stream` for TLS
  (so close_notify is the end of the stream, as a peek at the socket's bytes couldn't tell). `flush_before_read` gets a
  hold flag while the watcher reads, as flushing then would race with the handler writing the response. The end of the
  stream, or a reset, stops the connection's source; data ends the watch without stopping it.
- **Handing the read back**: when the handler returns, the server flushes the response itself, and takes over the
  watcher's pending read as its wait for the next head, rather than canceling it: `socket::set_read_deadline` applies
  to a read already waiting (retiming its timer under the shard's lock: `scheduler::retime` exists, for
  `handle_operation`s), and the server awaits the watcher's result (a `notifier`). Nothing's allocated per request,
  and the syscalls are the ones the server makes now (a `recv` that would block, then the one that reads); the cost is
  a hand-over between two coroutines on the pool per request, which `bench_request`'s socket test measures.
- The body readers must not touch the connection once at their end (`chunked_reader` keeps `done`; `limit_reader`
  stops at its limit): checked, and tested, as the watcher would race with them otherwise.

### Middleware and routers

The routers and `http_metrics`/`http_tracing` pass the request through, so the token reaches the handler without them
doing anything. What they record for a canceled request is Decided questions (7): the recommendation is 499 (nginx's
Client Closed Request, `status::CLIENT_CLOSED_REQUEST`) when the token was stopped by the client's side (not the
server's hard stop) before a final response was sent, in the metrics' `code` and the span's
`http.response.status_code`, the span not marked an error (it's the client's doing, as a 4xx is). Middleware that
wants its own deadline (a timeout handler, as Go's `TimeoutHandler`) makes a source of its own, linked to `req.stop`
by a `std::stop_callback`, swaps it into `req.stop` for the handler it wraps, and puts the original back: the field
being assignable is what allows it. A timeout middleware itself is later (it needs a timer task per request).

## Costs

- **HTTP/1.1: one allocation per connection** (its `std::stop_source`), nothing per request: setting `req.stop` is a
  token copy (two atomic reference-count changes), and the hard-stop callback is registered once per connection.
  `bench_request`'s in-memory tests don't go through the server, so they don't change; its socket test should show
  the same allocations per request (a few: the frames CLAUDE.md lists, `decode_reading`, `fill_after` and the
  handler's). Stage 4's watcher: one frame per connection, and a hand-over per request (to measure).
- **HTTP/2: one allocation per stream**, against the several a stream makes already (the `http2_stream`, its
  `stream_reader`, the handler's task and the scope's frame, the stream maps' nodes, header strings past the small
  buffers): about one more in ten, to be measured (a loopback count of allocations per stream, before and after,
  with `bench_request`'s counting allocator and the client's prior knowledge, as nothing measures HTTP/2 yet). If it
  shows, the source can be made lazily (only when the handler first asks, which needs an accessor: Decided questions (1)
  and (9)), or `std::inplace_stop_source` once the standard libraries have it.
- **Waits with a token**: a `std::stop_callback` registered and deregistered per wait (an atomic lock on the stop
  state, and a list insertion), no allocation; nothing for waits without one. Sockets: the same, only on the path that
  waits (after `EAGAIN`), and only for a socket given a token.
- **Stopping**: a vector of sources per connection, reserved, used only when streams fail.

## Testing

- **Scheduler** (`test/io/scheduler.cpp`): `sleep_until`, `wait_on` and `wait` with a token stopped before the wait,
  while it waits, and racing its completion (a loop, under TSan where available); a token that can't be stopped
  behaves as none; stopping wakes only that wait. Sockets: a read canceled while waiting, then a read that gets the
  bytes (nothing lost); a write likewise; `connect` canceled; `run_blocking` with a stopped token doesn't run `func`,
  and passes it to a `func` that takes one.
- **HTTP/2** (`test/http/http2_server.cpp`, with `h2_client.hpp`): a handler sleeping with `req.stop` wakes with
  `operation_canceled` when the client resets its stream; when the stream times out (its body stalled, the handler
  reading it in a child task and sleeping in another); when the connection ends (the client closing; ENHANCE_YOUR_CALM
  at the handler limit), and the group-5 test's second client (with `max_connections = 1`) is answered as soon as the
  canceled handlers return, rather than when they would have finished; the other streams of a connection go on after
  one's reset; the peer's GOAWAY doesn't stop running streams; graceful `close()` doesn't, `abort()` does, and an
  incomplete response is then reset with CANCEL. A debug-only check that `stop_pending()` is never called holding
  `mu`.
- **HTTP/1.1** (`test/http/server.cpp`): `abort()`, `~server` and `scheduler::request_stop()` stop a running
  handler's token; a token a handler kept is stopped once the connection ends. Stage 4: the client closing during a
  handler stops it (cleartext and TLS, with and without close_notify); a pipelined request arriving during a handler
  doesn't, and is served next; a request with a body being read isn't watched until its end; a response after the
  watch goes out, and the next request's head deadline holds.
- **Client** (`test/http/client.cpp`, `http2_client.cpp`): canceled waiting for a connection, while connecting, while
  waiting for the head and while reading the body; over HTTP/1.1 the connection isn't reused, over HTTP/2 the stream's
  reset (the server's handler sees its token stopped) and the connection is.
- **Middleware** (`test/instrument/middleware.cpp`): a canceled request is recorded as 499.
- `bench_request`'s numbers (time and allocations per request) before and after each stage, in the pull requests.

## Stages

1. **Waits that take a token**: the scheduler's overloads and `stoppable_wait_operation`, sockets'
   `set_stop_token` (and `tls::stream`'s, and `connect`'s option), `run_blocking(func, stop)`, and
   `scheduler::stop_token()`. Tests. Useful without HTTP, and what the next stages build on.
2. **The server's token**: `server_request::stop`; HTTP/2's per-stream sources, stopped outside the lock; HTTP/1.1's
   per-connection source; `abort()`, `join()`, the destructor and the scheduler's stop; handlers returning after a stop
   (resets, logging). The HTTP/2 allocation count. `docs/guide.md` (the server section's "which can't be cancelled", and
   the runtime's waits), CLAUDE.md, and the comments on `run()`/`schedule()` and in `http2.md` that say handlers can't
   be canceled.
3. **The client**: `client_request::stop`, through the pool's waits, connecting, HTTP/1.1 and HTTP/2.
4. **HTTP/1.1 noticing the client leave**: the watcher, `flush_before_read`'s hold, deadlines that apply to a read
   already waiting. Measured with `bench_request` before it's merged; if the hand-over costs too much, it's opt-in
   (a `server_config` setting) or dropped.
5. **Middleware**: 499 in `http_metrics` and `http_tracing`. Later: a timeout middleware, and a helper for linking
   sources.

## Decided questions

Each was decided as recommended (2026-10-18); the options are kept for the reasoning.

1. **The request's token: a field, or an accessor?** (a) `std::stop_token stop` as a public field, as the rest of
   `server_request` is; middleware can swap in a linked token for the handler it wraps. (b) `stop_token()`, a member
   function: allows making HTTP/2's source lazily (only for handlers that ask), at the cost of synchronizing the first
   call with a stop (through the stream's connection, as `response_writer`'s HTTP/2 back end does), and middleware
   can't replace it without another setter. (c) A third handler parameter: breaks every handler; no. Also its name
   (`stop`, `stop_token`, `cancel`). **Recommended: (a), named `stop`**, unless stage 2's count says HTTP/2's allocation
   matters (then (b)). Changing to `std::inplace_stop_token` later would change the field's type either way.
2. **What stops handlers when the server closes?** (a) Graceful `close()` stops them too (simplest; a draining HTTP/2
   connection's streams then end early, which undoes much of draining). (b) `close()` stays graceful; a new `abort()`
   stops them, as do `~server` and `scheduler::request_stop()` (Go's `Shutdown` and `Close`). (c) `close(grace)`: stops
   them after a grace period, which needs a timer task. **Recommended: (b)**; (c) is a few lines over it, in the
   application or later. Related, found here, and since fixed: HTTP/1.1's `close()` shut every connection's reading
   side at once, so a handler reading a request body found it cut short (and, framed by Content-Length, couldn't
   tell, as `limit_reader` passed its reader's end on as the body's: it now reports it as `connection_aborted`, as
   `chunked_reader` does), while HTTP/2's `close()` let streams finish. HTTP/1.1's `close()` now shuts only
   connections waiting for a head, and the others after their request (see Where things stand), so `close()` is
   graceful for both, and (b)'s line between `close()` and `abort()` is the same for both; `~server` already ends
   HTTP/1.1's requests in progress (`aborting`), as (b) has it.
3. **Should HTTP/1.1 notice a client leaving while its handler runs?** (a) No: only the server's stops (and the
   connection ending) stop its token; a client that leaves is seen when the response is written. (b) The watcher of
   stage 4 (Go's background read, through the connection's buffer: exact for TLS too; one frame per connection, a
   hand-over per request). (c) A readiness wait and `recv(MSG_PEEK)`: no read to hand back, but over TLS a close_notify
   is just bytes, so a client that sends one before its FIN isn't noticed. **Recommended: (b), as its own stage after
   measuring; (a) until then.**
4. **Should the handler learn why it was stopped?** (a) No: the token says only that it was; HTTP/2's body I/O
   already returns the stream's failure. (b) `server_request` gets the reason as an `std::error_code` (e.g.
   `connection_reset`, `timed_out`, `connection_aborted`, `operation_canceled` for the server's stop), set before the
   stop is requested. **Recommended: (a)** for now; (b) is additive.
5. **Outgoing requests: `client_request::stop`, or a parameter of `send()`?** A field travels with the request that
   `send()` takes by value, and covers reading the response's body. **Recommended: the field**, plus `get(url, stop)`.
6. **Sockets: a token per socket, or per operation?** (a) `set_stop_token`, as deadlines are set: the `io::reader`/
   `io::writer` interfaces stay as they are. (b) A token parameter on `read`/`write`: every reader and writer changes.
   **Recommended: (a).**
7. **How middleware records a canceled request.** (a) 499 when the client's side stopped it before a final response
   (metrics and spans; the span not an error). (b) As now (the handler's status, or 200 or 500). (c) A separate label
   or attribute (e.g. `error.type`). **Recommended: (a)**, as nginx's logs do.
8. **Implicit propagation**, as `std::execution::task`'s environment: `co_await scheduler.sleep_until(t)` inside a
   handler would be canceled without passing the token. It costs every `co_await` a copy and every promise a pointer,
   and spawned tasks would need a rule. **Recommended: not now**; explicit tokens can coexist with it later.
9. **The per-stream allocation**, if stage 2's count shows it: accept it, make it lazy (needs (1)(b)), or an in-house
   source embedded in the stream (no `std::stop_token`, so no interoperation). **Recommended: accept, and move to
   `std::inplace_stop_source` when libc++ and libstdc++ have it.**

## Out of scope

- Canceling by destroying a handler's coroutine (the runtime can destroy one waiting on an operation, but a handler's
  frame holds what it was doing, half done): canceling stays cooperative.
- Interrupting `run_blocking`'s function, or `getaddrinfo()`.
- Freeing a connection's `max_connections` slot before its handlers return (group 5's bound stays).
- Per-request deadlines for handlers that don't wait on their stream (HTTP/2's stream timeouts only run while the
  handler reads or writes it), and a timeout middleware: later, built on this.
- Tokens on `coro::notifier`, `async_scope::join` and `thread_pool::schedule`.
