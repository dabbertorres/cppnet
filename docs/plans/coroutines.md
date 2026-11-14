# Plan: one coroutine per sequence of actions

Status: decided (2026-11-01): every question at the end (see Decided questions). Stages 0 to 4 done (pull requests
#112 to #117; Learned in the stages has how each met the bar). Paused after stage 4 by the user (2026-11-02); the
`http11` references change (below) is done (#120). Stage 5 resumed with the user's go-ahead (2026-11-13).

## Goal

Coroutines used as cooperative scheduling: one coroutine (one frame) per sequence of actions (a connection, a stream,
a request, a WebSocket read that has to wait), where `co_await`ing a step of it costs no allocation, whether or not the
step waits. On every compiler (GCC never elides a frame) and at every optimization level, without depending on Clang's
heap allocation elision (HALO) heuristics. Each change is proven worthwhile by measurement before it's merged, or
dropped (the bar is Decided question 2).

The target per unit (both sides, where a bench runs both in one process):

| unit | today (frames / allocations) | target frames |
|---|---|---|
| HTTP/1.1 request over a socket | 3 / 3 | 1 (the handler's) |
| HTTP/1.1 client request (client and server) | 17 / 29 | 6 (`send()`, `send_http11()`, the server's handler, router and the bench's own; the client's chain is Decided question 12) |
| client body `read()` of buffered data | 2 / 2 | 0 |
| HTTP/2 stream, client and server | 9 / 37 | 6 (`send()`, `send_http2()`, `http2_client_connection::send()`, `wait_for_head()`, `run_stream()`, the handler) |
| WebSocket read that waits | 2 / 2 | 1 (`drive()`) |
| WebSocket 64 KiB message, one at a time | 9 (HTTP/1.1), 12 (HTTP/2) | 2 (the two reads' `drive()`) |
| TLS write that the socket takes / read that waits | 2 / 2 | 0 / 0 |
| HTTP/1.1 connection setup | 8 / 14 | 4 (`serve_connection`, `serve_requests`, `watch`, the handler) |
| `scheduler::spawn()` | 2 wrapper frames, and the task's | the task's |

## Where things stand

Measured on master 3528cf2 (2026-11-01): macOS x86-64 (Intel Core i9-9980HK), Homebrew Clang 23.1.3 and libc++ 23, the
`dev-clang` preset's Release flags (`-O3 -DNDEBUG`).

### How it was measured

- **Which allocations are frames.** The library rebuilt from `build-Release.ninja`'s commands, unchanged but for
  `-gline-tables-only` and `-Rpass=coro-elide -Rpass-missed=coro-elide`; copies of `bench_request`, `bench_client`,
  `bench_websocket` and `bench_tls` whose counting `operator new` also records a backtrace (inside the benches' own
  measurement windows only), symbolized with `atos -i`; an allocation is a frame when its innermost function is a
  coroutine (has a `.resume` clone). A throwaway program measured what no bench isolates (a TLS echo, a connection per
  request, a native DNS lookup against a fake nameserver). The traced counts equal the benches' own exactly.
- **What the frames' allocations cost.** A variant of `coro/task.hpp` whose `promise_base` has a class `operator
  new`/`delete` serving frames from per-thread free lists (64-byte classes to 16 KiB, at most 64 kept per class,
  frames freed on another thread joining its lists), out of line, nothing else changed: it removes the frames'
  `malloc`/`free` and nothing else. Both builds from the same commands, 5 rounds each, interleaved (alternating which
  goes first), each run started only with the 1-minute load below 4.

### Where the frames are

- **Socket waits are already operations** (`coro::operation`: no frame), as is an HTTP/2 stream's read that waits, and
  functions that usually finish without waiting return `ready_task`s. The frames left come from the layers between:
  - **Wrappers whose waiting path returns a coroutine's task**: `buffered_reader::fill_after` (208 B, under every read
    that waits), `buffered_reader::read_more`, `buffered_writer::flush_after`/`write_through`,
    `limit_reader::count_after`, `limit_writer::finish_after`, `flush_before_read::flush_and_read`, the server's
    `sent_after`, the WebSocket's `flush_waiting`, TLS's `connection::read_more` and `stream::write`'s lambda. The task
    escapes, so no compiler can elide its frame.
  - **Readers and writers that are coroutines even for buffered data**: `chunked_reader::read` (920 B) and the client's
    `pooled_body::read` (232 B), both called through `io::reader`; `string_reader::read`, which never waits.
  - **Spawns**: `scheduler::spawn()` wraps the task in `start_on()` (120 B) and `async_scope::spawn()` in `run()`
    (96 B): two frames besides the task's own.
  - **Type-erased handlers** (called through `request_handler`), and a few loops (`receive()` in TLS's `read_more`,
    `send_http11()`/`send_http2()` in `client::send()`'s retry loop).
- **Frames are almost everything allocated on socket-bound paths**:

  | path | allocations per unit | frames | which |
  |---|---|---|---|
  | HTTP/1.1 request over a socket | 3 | 3 | the handler (472 B), `decode_reading` (248 B), `fill_after` (208 B) |
  | HTTP/1.1 requests, pipelined | 1.4 | ~1.2 | the handler; per batch of 16, `fill_after` and `decode_reading` |
  | HTTP/1.1 connection setup | 14 | 8 (14.5 KB) | `accept` 936 B, `serve_connection` 5048 B, `serve_requests` 7520 B, `watch`, `room_for_connection`, `start_on`, `run`, the handler |
  | HTTP/1.1 client request (both sides) | 29 | 17 | the client's `send` 9856 B, `send_http11` 10456 B, `acquire`, `request_encode` 1728 B, `response_decode` 2648 B and four more under it, `fill_after`, `pooled_body::read` x2; the server's 4 |
  | client chunked body, a 256-byte `read()` | 2 | 2 | `pooled_body::read`, `chunked_reader::read`: 66,600-74,300 allocations per 8 MiB |
  | HTTP/2 stream, a stream at a time (both sides) | 37 | 9 | `send`, `send_http2` 11112 B, `http2_client_connection::send` 5320 B, `wait_for_head` 1960 B, `acquire`, `run_stream`, `run`, the handler (the rest: HPACK field vectors, map and hash nodes, `wait_on()`'s `std::map` node, `notify()`'s vector, the stream objects) |
  | WebSocket message, one at a time | 4 | 4 | two reads that wait, each `drive<>` (712 B) and `fill_after` |
  | WebSocket 64 KiB message, one at a time | 9 (HTTP/1.1), 12 (HTTP/2) | all | as above, and `read_more` x2, `write_through`, `send_through`, `send_masked` (HTTP/2: `flush_after`, `flush_waiting`, the stream's `write_waiting`) |
  | TLS write of 16 B that the socket takes | ~6 | 2 | `stream::write`'s lambda (552 B), `write_all` in `write_queued` (320 B); the rest Botan's record buffers and the ciphertext vector |
  | TLS read that waits | ~4 | 2 | `connection::read_more` (168 B), `receive` (608 B); the rest Botan's |
  | TLS 1.3 full handshake (both sides) | ~1,900 | ~23 (1%) | Botan's are the rest |
  | native DNS lookup, one family | 33 | 8 | `look_up`, `run_lookup`, `native_lookup`, `ask`, `ask_servers`, `send_udp`, `start_on`, `run` (~100 us each) |

- **GCC elides nothing**: the 71 calls Clang elides in the library are allocations there (e.g. each TLS write's
  `flush()` and `write_queued()`, `chunked_writer`'s, `write_all`'s variadic tail).
- **A frame allocator interacts with HALO**: the pool variant with its `operator new` inlined into the ramps lost
  elisions (`server.cpp` 5 to 1, `tls.cpp` 14 to 12: the ramps grew past the inliner's threshold, so their callers
  never saw them); out of line (`[[gnu::noinline]]`), every elision stayed.
- **Which calls Clang elides** (Clang 23, `-O3`, checked with `-Rpass=coro-elide -Rpass-missed=coro-elide`): by
  default (`coro-elide`) only a coroutine called directly in a `co_await`, and not inside a loop; never through a
  wrapper that isn't a coroutine (e.g. one returning a `ready_task` or a coroutine's task), nor an indirect call (e.g.
  the handler). `[[clang::coro_await_elidable]]` on `task` switches to another pass (`coro-annotation-elide`, remarks
  `-Rpass=coro-annotation-elide`) that elides calls by how often they run: those in loops, but not ones that run once,
  unless `-mllvm -coro-elide-branch-ratio=0` (an internal LLVM option). So the frames a request that has to wait still
  allocates (the handler's, and `decode_reading`'s for a head that comes in pieces) aren't elided. The attribute could
  take `decode_reading` (with `serve_connection` awaiting it directly, defined in a header), at the cost of bigger
  frames (eliding cold calls would put e.g. the ~4.5 KB rare-path coroutines in each connection's frame) and an
  unstable flag (question 9: skipped); a frame allocator (question 8) is the other option.
- **`task`'s destructor is always inlined** (`task.hpp`'s note says why): before, a virtual call in it lost 77 of 78
  elisions, which had been put down to the call being opaque. Inlined, what it calls may be opaque.

### What the allocations cost

The pool variant against master (medians of 5 rounds; allocations exact; the pool's count only what isn't a frame):

| bench, case | master | pool | change | allocations master / pool |
|---|---|---|---|---|
| `bench_task`, a `co_await` of a task that finishes at once (flat / nested) | 51.67 / 35.3 ns | 34.8 / 26.56 ns | -32.6% / -24.8% | 1 frame each |
| `bench_request`, pipelined | 1131.7 ns | 1026.7 ns | -9.3% | 1.4 / 0.2 |
| `bench_request`, one per read | 1346.2 ns | 1150.0 ns | -14.6% | 4.2 / 0.2 |
| `bench_request`, the runtime router | 1267.9 ns | 1118.1 ns | -11.8% | 2.4 / 0.2 |
| `bench_request`, over a socket | 29.0 us (28.0-30.2) | 28.0 us (27.1-29.3) | -3.4% (noise) | 3.0 / 0.0 |
| `bench_request`, over HTTP/2, a stream at a time | 67.1 us | 66.1 us | -1.5% (noise) | 37.0 / 29.0 |
| `bench_request`, 8 streams at once | 920.0 us | 872.5 us | -5.2% | 56.0 / 36.1 |
| `bench_client`, small, 4 KiB reader | 43.79 us | 42.07 us | -3.9% | 29.0 / 12.0 |
| `bench_client`, chunked download, 1 / 4 / 16 KiB readers | 18197 / 11436 / 9677 us | 13684 / 7682 / 6150 us | -24.8% / -32.8% / -36.4% | 74270 / 68126 / 66589 to 10 |
| `bench_websocket`, HTTP/1.1, 16 B, one at a time | 27.68 us | 27.05 us | -2.3% (noise) | 4.0 / 0.0 |
| `bench_websocket`, HTTP/2, 64 KiB, one at a time | 105.27 us | 103.45 us | -1.7% (noise) | 12.0 / 0.0 |
| `bench_tls writes`, 16 KiB | 36.6 us (36.3-36.9) | 37.8 us (37.1-37.9) | +3.3% | 10.83 / 6.94 |

- **A frame's `malloc`/`free` costs about 17 ns** on this machine (`bench_task`).
- **Where frames are per read of buffered data they dominate** (the client's chunked reads: -25% to -36%), and the
  server's cases in memory gain 6% to 15%.
- **Socket-bound cases move within noise by wall time** (a loopback round trip is ~28 us of the kernel and task
  hand-overs), but **by CPU time they move**: the whole `bench_websocket` process, 3 interleaved runs each, user CPU
  16.41-16.71 s (master) against 15.27-15.41 s (pool), -7.5%, system time unchanged (28.35-28.99 s). So socket cases
  need CPU time (or throughput under load) to show a gain; the benches print only wall time today (stage 0).
- **A pool isn't free**: TLS writes were 1-3% slower in every round (plausibly frames freed on the other worker, and
  cold 16 KiB classes: unverified).
- **Removing a frame outright** should gain at least what pooling it does on the same cases: it also skips the frame's
  initialisation, its resume and destroy paths, and its cache footprint, without a pool's costs.

## How others do it

- **Rust**: an `async fn`'s future embeds its callees' futures by value, so a whole await chain is one state machine
  and allocates nothing at an `.await` ([reference](https://doc.rust-lang.org/reference/items/functions.html)); boxing
  only for recursion ([E0733](https://doc.rust-lang.org/error_codes/E0733.html)) and `dyn`
  ([async-trait](https://docs.rs/async-trait/latest/async_trait/)), and Tokio allocates once per spawned task
  ([core.rs](https://github.com/tokio-rs/tokio/blob/master/tokio/src/runtime/task/core.rs)). The price is size
  (`clippy::large_futures`, [rust-lang/rust#62958](https://github.com/rust-lang/rust/issues/62958)). C++ can't lay out
  a callee's frame in its caller's (its size is known only once the callee is compiled), but the shape (one allocation
  per spawned unit, the steps under it allocating nothing) is the goal here.
- **Senders** ([P2300R10](https://wg21.link/p2300r10)): `connect()` makes an operation state its caller stores inline,
  so a composed chain allocates nothing ([P2006](https://wg21.link/p2006r1) introduced the split). P2300 calls a
  coroutine's frame "an unavoidable dynamic allocation" in many cases. `coro::operation` is the same idea for I/O
  leaves: state in an object that already exists, started when awaited.
- **Asio**: `awaitable` frames come from a per-thread recycling cache, 2 slots per purpose, blocks up to ~1 KiB
  ([awaitable.hpp](https://github.com/chriskohlhoff/asio/blob/master/include/asio/impl/awaitable.hpp),
  [thread_info_base.hpp](https://github.com/chriskohlhoff/asio/blob/master/include/asio/detail/thread_info_base.hpp));
  `co_composed` frames from the handler's associated allocator. No published numbers.
- **Boost.Capy**: a thread-local "current frame allocator" read by the promise's `operator new` (no `allocator_arg`
  threaded through signatures), the resource stored at the frame's end, power-of-two classes of 64-2048 B with 16 per
  thread bucket ([frame_alloc_mixin.hpp](https://github.com/cppalliance/capy/blob/develop/include/boost/capy/ex/frame_alloc_mixin.hpp)).
- **Seastar**: the coroutine's promise *is* the scheduled task, so frame and task are one allocation
  ([coroutine.hh](https://github.com/scylladb/seastar/blob/master/include/seastar/core/coroutine.hh)).
- **folly::coro, cppcoro, libunifex**: global `new` for task frames (folly through no-inline wrappers for profiling,
  [Malloc.h](https://github.com/facebook/folly/blob/main/folly/coro/detail/Malloc.h)); libunifex's concrete sender
  chains store operation states inline, its type-erased senders allocate per `connect`.
- **Clang's `[[clang::coro_await_elidable]]`** (Clang 20) is "a hint"
  ([AttributeReference](https://clang.llvm.org/docs/AttributeReference.html)): the callee must be defined in the same
  translation unit, the call direct, and since LLVM 22 on a path at least 0.55 as frequent as the caller's entry
  ([CoroAnnotationElide.cpp](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Transforms/Coroutines/CoroAnnotationElide.cpp));
  never at -O0. folly reports 10-20% from it
  ([Discourse](https://discourse.llvm.org/t/language-extension-for-better-more-deterministic-halo-for-c-coroutines/80044));
  Clang's coroutine maintainer reports more elision mostly regressing in their tests, as elided frames add up in the
  root's ([article](https://chuanqixu9.github.io/c++/2026/03/27/C++20-Coroutines-from-compiler-and-library-authors-perspective.en.html)).
- **GCC** has no frame elision at all ([bug 100579](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=100579), open since
  2021).
- **Portable** (GCC and Clang, any optimization level): a promise `operator new`/`delete` (optionally with a leading
  `std::allocator_arg_t`), per-thread size-class free lists, the frame as the task, and operations whose state lives
  in an object that exists.

## C++26

| paper | what | in C++26 | libc++ 23 | libstdc++ 16 |
|---|---|---|---|---|
| [P2300R10](https://wg21.link/p2300r10) | `std::execution` (senders and receivers) | yes, St. Louis 2024 | no | no |
| [P3552R3](https://wg21.link/p3552r3) | `std::execution::task` | yes, Sofia 2025 | no | no |
| [P3980R1](https://wg21.link/p3980r1), P3941R4, P3927R2, P4151R1 | `task`'s fixes (allocator, scheduler affinity, ...) | yes, Croydon 2026 | no | no |
| [P2079R10](https://wg21.link/p2079r10) | `parallel_scheduler` | yes, Sofia 2025 | no | no |
| [P3149R11](https://wg21.link/p3149r11) | `async_scope` | yes, Sofia 2025 | no | no |
| [P2477R3](https://wg21.link/p2477r3) | controlling coroutine elision | no (stalled since 2023) | - | - |

Support: [cppreference's C++26 page](https://en.cppreference.com/cpp/compiler_support/26) lists them all unsupported
in both libraries; libc++'s [status CSV](https://github.com/llvm/llvm-project/blob/main/libcxx/docs/Status/Cxx26Papers.csv)
leaves them empty (23.1.3's `<version>` has `__cpp_lib_senders` commented out), and GCC 16's
[version.def](https://github.com/gcc-mirror/gcc/blob/releases/gcc-16/libstdc%2B%2B-v3/include/bits/version.def)
defines none. [NVIDIA stdexec](https://github.com/NVIDIA/stdexec) is the reference implementation (GCC 12+, Clang 16+).

`execution::task` allocates a frame per call, as `coro::task` does (P3552R3 4.6: "the HALO optimisations aren't always
possible"). It is allocator-aware (a leading `std::allocator_arg` allocator, required first by P3980R1, else the
environment's `allocator_type`), resumes on its own scheduler by default (a hop cppnet doesn't make), and has no
symmetric transfer ([P3801R0](https://wg21.link/p3801r0), open). So C++26 makes nothing here obsolete. What it
suggests is the direction this plan takes anyway: I/O leaves as operations (state stored in place, started, completed,
canceled), which could later be wrapped as senders, rather than adopting `execution::task` (Decided question 10).

## Design

### An adapter operation

One generic template in `coro/` (Decided question 3): an operation that an object holds (one per object, as a socket
holds one read and one write operation), made by the function that today returns `X_after(std::move(inner))`.
Awaited, it starts `inner`: if `inner` is itself an operation (a socket's read that waits), it forwards the awaiting
coroutine to it, so the socket resumes the caller directly and nothing runs between; if it's a coroutine's task (a TLS
stream's read), it awaits it as `task`'s awaiter does. `result()` takes the inner result and applies the owner's
finishing step (`filled()`, `counted()`, ...). Claimed once (`device_or_resource_busy` otherwise, as a socket's), and
canceled with the awaiting coroutine (`operation_base::cancel()`). It replaces `buffered_reader::fill_after` and
`read_more`, `buffered_writer::flush_after` and `write_through`, `limit_reader::count_after`,
`limit_writer::finish_after`, `flush_before_read::flush_and_read`, the server's `sent_after` and the WebSocket's
`flush_waiting`.

A function that awaits several inner tasks in turn (`read_more`'s and `write_through`'s loops, `flush_after`'s writes,
`flush_and_read`'s flush and then read, the WebSocket's frame written in parts and its flushes) can't hand the awaiting
coroutine to its inner task, as something must take the steps between them. `coro::stepper` (stage 3) is the adapter for
those: its step makes of each inner task's result its own result, or the next inner task; steps whose inner tasks finish
at once are taken where they finish, and the inner tasks that wait continue the stepper's driver, a coroutine it makes
the first time one waits and keeps, parked between its tasks (as the HTTP/1.1 server's watcher is), which takes the
steps and then resumes the awaiting coroutine. So one frame per object, made once, rather than one per call, and one
more resume per wait. A step can't let go of what keeps the stepper's users apart (the WebSocket's wire) while the
stepper's still its task's: that's left to the stepper's `Released` hook, run once it's free.

### Ready tasks where nothing waits

`string_reader::read` (never waits); `chunked_reader::read` and the client's `pooled_body::read` when the inner read
finished without a coroutine (the bookkeeping then done inline); `connection_pool::acquire` with an idle connection;
`room_for_connection` below the limit; `write_all` when each write finished without a coroutine; the client's head
encoding and decoding when the head fits or is buffered, as the server's `response_encode` and `decode_buffered` are.

### Decode after the wait

The HTTP/1.1 server's loop (and the client's response read) awaits the fill (an operation), then decodes a buffered
head without a coroutine; `decode_reading` stays for heads that arrive in pieces.

### Operations in long-lived objects

`listener::accept()` (one accept at a time), the HTTP/2 stream's write that waits, TLS's read (`read_more` and
`receive`: one reader at a time) and its flush, and spawning without wrapper coroutines: the task's promise is the
detached task (Decided question 4), counting itself in the scope, resumed on the pool before it runs, and destroying
itself when it ends, as Seastar's is.

### What stays a frame

One per sequence: a connection's (`serve_connection`, and `serve_requests`, kept apart on purpose), a stream's
(`run_stream`), a request's on the client (`send()` and the chain under it, Decided question 12), a handler call
(type-erased), a WebSocket read that waits (`drive()`). Those are what a frame allocator would serve, if stage 6
adopts one.

## Measuring

Every stage measures before and after, against the bar (Decided question 2), and its pull request gives a verdict per
metric of each benchmark case: a gain or a regression, or noise (ignored), told apart by a Mann-Whitney U test on the
metric's interleaved rounds (p < 0.05; from 0.05 to 0.2, more rounds, up to about 10 a side, then the test again;
Decided question 2), with the medians, ranges and p in the pull request.

- **Allocations** are exact: each stage states the counts its cases should reach (from Where things stand).
- **Time**: the stage's cases, master and the branch built from the same commands, 5 rounds each, interleaved
  (alternating which goes first), each run started only with the 1-minute load below 4; medians, with every round
  listed in the pull request. Wall time for cases in memory (pipelined, one per read, the
  middleware cases, the client's chunked reads, `bench_task`); **CPU time per unit** for socket cases (stage 0 adds it),
  and `bench/web` (cppnet's server alone) for throughput under load (Decided question 5).
- **Frame sizes**: Clang's `-Rpass=coro-elide` remarks give each frame's size (`frame_size=`), and the elided calls'
  count before and after, so a change that costs an elision elsewhere, or grows a frame it keeps, shows.
- **GCC**: the cases' allocation counts on GCC (no elision) at stage 0 and after stage 5, in a throwaway Linux
  container (Decided question 7).

## Stages

Each a pull request, in this order, with its benchmark cases, baselines (Where things stand) and the bar.

0. **Benches.** No library change: the cases no bench isolates, so every later stage has its proof in place.
   - `bench_tls echo`: 16 B and 16 KiB round trips over one TLS 1.3 connection (24 allocations per round trip, 8 of
     them frames; ~33 / ~139 us).
   - `bench_request`: a connection per request (accept, one request, close; a blocking client): 14 allocations per
     connection, 8 of them frames; ~100 us.
   - CPU time per unit beside wall time on the socket cases of `bench_request`, `bench_client` and `bench_websocket`.
   - Master's baselines (every round), `bench/web`'s for cppnet's server, and GCC's allocation counts, in the pull
     request.
1. **The adapter operation, and `buffered_reader::fill()`** with it. Cases: `bench_request` over a socket 3.0 -> 2.0,
   one per read 4.2 -> 3.2; `bench_websocket` one at a time 4.0 -> 2.0 per message (both versions); `bench_client`
   small 29 -> 27. One frame of the four the pool served on one per read (-14.6% for all four) is about -3 to -4%,
   within that case's spread (about 2%): CPU time on the socket cases is the likely proof, or stages 1 and 2 together.
2. **Decode after the wait** on the server, and the client's response head. `bench_request` over a socket 2.0 -> 1.0,
   one per read 3.2 -> 2.2; `bench_client` small 27 -> 21 (the client's `response_decode`, `read_line`,
   `read_terminated_line`, `peek` and `parse_fields`, and the server's `decode_reading`); `bench/web` before and after.
   Target cases (named before measuring, 2026-11-01; gains are claimed only on these): `bench_request` one per read
   (time), and over a socket, a request at a time (time and CPU time); `bench_client` small with each reader size (1,
   4 and 16 KiB: time and CPU time); `bench/web` plaintext (256 and 1024 connections) and JSON (64 and 256
   connections), requests per second.
3. **The other adapters** (`read_more`, `write_through`, `flush_after`, `count_after`, `finish_after`,
   `flush_and_read`, `sent_after`, `flush_waiting`), and the WebSocket client's large-frame paths (`send_through`,
   `send_masked`). Cases, after stage 1: `bench_websocket` 64 KiB one at a time 7 -> 2 (HTTP/1.1) and 10 -> 3 (HTTP/2;
   the stream's `write_waiting` is stage 5), and the in-flight medium and large cases; `bench_request` 8 streams 56 ->
   ~52.6, 8 request bodies 91.5 -> ~75.5. Baselines: 64 KiB one at a time 87.94 / 105.27 us, 8 streams 920 us per
   round, 8 bodies 1288.6 us per round. Rechecked on master bbed316 (after stage 2): the same counts (64 KiB one at a
   time 7 and 10, in flight 7.04 and 10.48; HTTP/2 medium one at a time 7, as its 8 KiB messages take the large-frame
   paths through 4 KiB tunnel buffers; 8 streams 56.0, 8 bodies 91.5). `read_more` also goes from `bench_client`'s
   chunked downloads, each 256-byte read that refills the buffer (74,263 / 68,119 / 66,584 per 8 MiB with 1 / 4 /
   16 KiB readers). Target cases (named before measuring, 2026-11-01; gains are claimed only on these):
   `bench_websocket`, one WebSocket: HTTP/1.1 large (64 KiB) one at a time, medium (8 KiB) and large in flight; HTTP/2
   medium and large, one at a time and in flight (time and CPU time); `bench_request` over HTTP/2, 8 streams at once and
   8 request bodies at once (time and CPU time); `bench_client` chunked downloads with each reader size (1, 4 and
   16 KiB: time and CPU time).
4. **Ready tasks** (`chunked_reader::read`, `pooled_body::read`, `string_reader::read`, `acquire`,
   `room_for_connection`, `write_all`, the client's head encoding). Cases: `bench_client`'s chunked downloads 66,600-
   74,300 -> under ~1,000 allocations per 8 MiB (with stage 3's `read_more`), and their time (18197 / 11436 / 9677 us
   with 1 / 4 / 16 KiB readers; the pool measured -25% / -33% / -36%, so removing the frames should gain at least as
   much); small 22 -> ~18; `bench_request` 8 bodies ~75.5 -> ~59.5, a connection per request -1, HTTP/2 a stream at a
   time -1. Rechecked on master 06f7d06 (after stage 3), with the allocation tracer: chunked downloads 65,942 per
   8 MiB with each reader (stage 3 took `read_more`'s frames): `pooled_body::read` and `chunked_reader::read` for each
   256-byte read (65,538), and for each 64 KiB chunk `read_chunk_size` and the bench handler's `write_all` (129 each);
   small 21, not 22 (as stage 2 left it), of which `acquire`, `request_encode` and its head's string,
   and two `pooled_body::read`s go; long head lines 56 / 37 / 30, the same 5 going; a connection per request 15, not 14
   (`room_for_connection` goes); HTTP/2 a stream at a time 37 (`acquire` goes); 8 bodies 75.5 (16 `string_reader::read`s
   and `acquire` go: ~58.5); 8 streams 52.7 (the bench handler's 4 `write_all`s and `acquire` go: ~47.7). Also touched:
   `write_all` in a TLS stream's `write_queued()` (`bench_tls writes` 10.83 per 16 KiB write, -1 per write; `bench_tls
   echo` 22 per round trip, -2), and the in-memory `bench_request` cases whose input is a `string_reader` (pipelined,
   and its middleware cases: 2 frames per batch of 16, -0.125 per request). Target cases (named before measuring,
   2026-11-02; gains are claimed only on these; the rule: every case whose allocation count drops, by the trace above
   and a probe run of each bench, whose printed times were seen but not used): `bench_client`, every case (small, long
   head lines and chunked download, with each reader size: time and CPU time); `bench_request`, a connection per
   request, over HTTP/2 a stream at a time, 8 streams at once and 8 request bodies at once (time and CPU time), and
   pipelined, through `http_metrics`, `http_metrics` on the route, the runtime router, the runtime router with
   `http_metrics` on the route, and `http::timeout` on the route (time); `bench_tls echo`, 16 B and 16 KiB (time and
   CPU time); `bench_tls writes`, each size (time). `bench/web` is measured as well (the server's accept loop changes,
   once per connection), but isn't a target. `write_all` was left a coroutine at first: making one piece the writer's
   own write made `bench_tls writes` 16-19% slower at every size and `bench_tls echo`'s 16 KiB round trip 13% slower,
   which a confirmation run reproduced. The cause was where Botan's code landed, not the forwarding: Botan's TLS 1.3
   constant-time padding scan (inlined in `Record_Layer::next_record`, about 23 us per 16 KiB record) ends in a fused
   `add; jb`, which Intel's JCC erratum (SKX102) slows by about 25% when it crosses or ends on a 32-byte boundary, and
   the forwarding, by shrinking the code linked before Botan, moved it onto one. Since `ports/botan/` builds Botan with
   `-falign-loops=64` on x64, the scan's place no longer depends on what's linked before it, and the forwarding came
   back (with the single-piece test): the TLS stream's `write_all` frame goes (`bench_tls writes` 10.83 -> 9.83 per
   16 KiB write, `bench_tls echo` 22 -> 20 per round trip), and so do the bench handlers' (chunked downloads ~274-277
   -> ~146-149 per 8 MiB, 8 streams 51.3 -> 47.2). Its target cases (named before measuring, 2026-11-03; the rule: every
   case whose allocation count drops, by a probe run of each bench, whose printed times were seen but not used):
   `bench_tls echo`, 16 B and 16 KiB (time and CPU time); `bench_tls writes`, each size (time), whether alone or in the
   full run; `bench_request` over HTTP/2, 8 streams at once (time and CPU time); `bench_client` chunked downloads with
   each reader size (time and CPU time). `bench_websocket` has none (its counts drift between runs, by timing, and it
   doesn't use `write_all`), nor do `bench_tls`'s handshakes (which print no counts).
5. **Operations in long-lived objects**: accept, spawn without wrappers (or keeping them, if the promise being the
   task complicates `async_scope`'s lease and join rules: Decided question 4), the HTTP/2 stream's write that waits,
   TLS's read and flush. Cases: a connection per request ~13 -> ~10; HTTP/2 a stream at a time -1, 8 streams ~52.6 ->
   ~49, 8 bodies ~59.5 -> ~45; `bench_websocket` HTTP/2 64 KiB 3 -> 2; `bench_tls echo` 24 -> ~16 per round trip;
   `bench_tls writes` 10.8 -> ~7 per 16 KiB write (36.6 us: the pool made it 1-3% slower; this mustn't). A pull request
   per object, each measured and kept or dropped on its own (the user, 2026-11-13), in this order: accept, spawning,
   the HTTP/2 stream's write, TLS's read and flush. Measured on the same Mac as stages 0 to 4 (the user, 2026-11-13).
   Then GCC's allocation counts again. Accept's target cases (named before measuring, 2026-11-13; gains are claimed
   only on these; the rule: every case whose allocation count drops, by a probe run of each bench, whose printed times
   were seen but not used, and, for `bench_tls`'s handshakes, which print no counts, a copy of it counting per
   connection): `bench_request`, a connection per request (14 -> 13 per connection; time and CPU time); `bench_tls`'s
   handshakes, with each server key, full without tickets, full and resumed, and TCP alone (one fewer per connection,
   e.g. TCP alone 10 -> 9, ECDSA P-256 full 1912 -> 1911; time and CPU time). `bench_client`, `bench_websocket`,
   `bench_tls echo` and `writes` keep their counts (their accepts are outside what they count; the in-flight and
   chunked cases' drift by timing, as before), and `bench/web` is measured as well (an accept per connection, which
   live through each run), but isn't a target. Spawning's target cases (named before measuring, 2026-11-13; the same
   rule, by a probe run of each bench and the counting copy of `bench_tls`): `bench_request`, a connection per request
   (13 -> 11 per connection: `start_on` and `run` around `serve_connection`), over HTTP/2 a stream at a time (36 ->
   35: the server's `run` around `run_stream`), 8 streams at once (47.6 -> 44.7 per response) and 8 request bodies at
   once (58.5 -> 55.6: the server's `run`, and the bench's `start_on` and `run` around each stream's sender), time and
   CPU time. `bench_client`, `bench_websocket` and `bench_tls` (handshakes, echo and writes) keep their counts (their
   spawns are outside what they count), as do `bench_notifier` and `bench_blocking` (which spawn only to set up).
   Measured as well, not targets: `bench_task`, as every `task<void>`'s final suspend now tests whether it was spawned,
   and `bench/web` (a spawn per connection, which live through each run).
6. **The frame allocator**, with what's left (handlers, the client's `send()` chain, `drive()`, the spawned frames):
   per-thread size-class free lists in the promise's `operator new`/`delete`, out of line (Decided question 8, asked
   again here with the data: the pool variant measured on top of stage 5, against the bar). Then the client's request
   chain is looked at again (Decided question 12).

Out of scope, for a separate plan (Decided question 11): the allocations that aren't frames (HTTP/2's 28 per stream:
HPACK field vectors, `wait_on()`'s `std::map` node and `notify()`'s vector, the stream maps and objects; the client's
reader and writer buffers made per request; TLS's ciphertext vector per flush).

## Learned in the stages

- **Stage 0** (#112): the benches and baselines above, and GCC's allocation counts.
- **Stage 1** (#113): kept. Ten interleaved rounds a side, five gains (one per read 7.1% faster, p = 0.001), no
  regressions.
- **Stage 2** (#114): allocations down, and `bench_client` small 3.1 to 3.5% faster (a target), but `bench_request`
  pipelined (not a target) 1.5 to 3.1% slower, reproduced three times. A separating build (the new library under the
  old bench loop) showed the code that case runs byte for byte the same, only moved within cache lines by a function
  added to `http11.cpp`. The user kept the stage, taking placement as the explanation: a confirmed regression on
  identical code whose addresses only moved is brought to the user with that evidence, and placement is an accepted
  reason.
- **Stage 3** (#115): kept (two target CPU-time gains, no regressions). `coro::stepper` costs one 56-byte driver
  coroutine per object, about 400 bytes more per HTTP/1.1 connection; a hand-built fake frame instead was rejected as
  undefined behaviour.
- **Stage 4** (#116): kept (gains on the client's chunked downloads, small requests and long head lines, and 8 request
  bodies; CPU time on 8 streams; no regressions), shipped first without `write_all`'s single-piece forwarding, which
  made TLS writes 16 to 19% slower. The user accepted two behaviours it brought: `connection_pool::acquire()` claims a
  free connection or stream when it's called, not when awaited (a task never awaited gives the lease back), and a
  `std::bad_alloc` while encoding a request's head returns `not_enough_memory`. The slowdown was then traced to
  Botan's padding scan and Intel's JCC erratum (stage 4's entry above); the user chose to build Botan with
  `-falign-loops=64` on x64 only (not per CPU generation: vcpkg's binary cache doesn't key on the CPU), documented in
  the portfile, and to bring the forwarding back. #117 did both: the flag against master reproduced no regression
  (the handshakes' medians leaned 1 to 4% slower, as plain TCP's did; pooled over 20 rounds, two handshake metrics
  gave p = 0.049 and 0.031, reported, not acted on), and Botan's text grew 3.4%; the forwarding against the flag cut
  allocations on every target case (above) with no p < 0.05 either way.
- **After stage 4** (approved by the user, 2026-11-02, as its own pull request): every `http11` encode and decode
  parameter (`request_encode`, `request_encode_head`, `request_encode_body`, `response_encode`, both
  `request_decode`s, `response_decode`) becomes a reference, its null check dropped; results stay pointers. Done
  (2026-11-13), with the private helpers they pass them to, `response_encoder` and the unused request aliases (the
  owning `response_decode` and `response_decoder` stay as they were). Ten interleaved rounds a side against master
  105d7b1 (`bench_request`, `bench_client`, `bench/web`): no allocation changed; six in-memory `bench_request` cases
  1.7 to 2.9% slower (p <= 0.017), of which a confirmation run of ten more reproduced two (one per read +0.8%, p =
  0.005; the runtime router +2.6%, p = 0.002). The code they run is the same but for `response_encode`'s dropped
  null test (`request_decode` and `set_request_body` byte for byte), and 2,431 functions after `encode_request` sit
  48 bytes earlier. The user kept it (2026-11-13), taking placement as the explanation, as in stage 2.
- **Stage 5, accept** (2026-11-13): `listener::accept()` takes a connection that's waiting as a ready task, and
  otherwise waits through an operation the listener holds, one accept at a time (another that has to wait fails with
  `device_or_resource_busy`, as it did before through the scheduler's wait), so it's no longer `const`. Allocations: a
  connection per request 14 -> 13, and `bench_tls`'s handshakes one fewer per connection; no other count changed, nor
  did any elision or kept frame's size (`-Rpass=coro-elide` over `src/` and `bench/`: 78 elided, 422 not, before and
  after; accept's own 936-byte frame is gone). Ten interleaved rounds a side against master 55c8542 (`bench_request`,
  `bench_client`, `bench_tls`, `bench/web`): one target gain, `bench_tls`'s ECDSA P-256 full handshake without
  tickets (time -1.0%, p = 0.031; CPU time -1.3%, p = 0.026), which ten more rounds didn't show again (p = 0.41 and
  0.27); a connection per request -4.8% and -3.4% (p = 0.68 and 0.57: noise); TCP alone leaned +6.6% (p = 0.058 and
  0.082, at ten rounds: noise; -1.3% in the next ten). Gains off the targets (reported, not claimed): the 4 KiB
  reader's chunked download (-2.0%, p = 0.005) and http::timeout over a socket (-3.5%, p = 0.034). One regression:
  `bench_client`'s long head lines, with each reader, 2.3 to 4.5% slower by time and CPU time (p <= 0.001),
  reproduced by a confirmation run of ten more (2.5 to 4.0%, p <= 0.007). The code it runs is unchanged and only
  moved: the two binaries' functions are the same instruction for instruction but for the listener's, the server's
  that use its layout (it holds the listener, now 272 bytes larger) or its accept loop, and padding after functions;
  `http11.cpp`'s functions sit 32 bytes earlier, the buffered reader's and the scheduler's mostly 144 bytes later.
  Verdict on the bar: an allocation drop and a target gain, and one regression on code that only moved. The user kept
  it (2026-11-13), taking placement as the explanation, as in stage 2 and after stage 4.

## Decided questions

All decided 2026-11-01.

1. **Direction.** (a) Structural first: stages 1-5 remove frames (operations, ready tasks), then a frame allocator is
   decided with what's left (stage 6); (b) a frame allocator first; (c) both at once; (d) neither. **Decided: (a).**
2. **The bar for "worthwhile".** In the user's words: "Any allocation drop and any gain. If a change results in no gain,
   but also no regression, then judge the change on maintainability and readability: if that is improved, keep the
   change; otherwise the change may be dropped. If unsure, ask me, showing evidence." So: an allocation drop or any
   gain, with no regression, means keep; with no gain and no regression, keep only if maintainability or readability
   improved, otherwise it may be dropped; when unsure, ask the user, with the evidence. The user's clarification
   (2026-11-01): "provable noise should be ignored when it comes to performance gains and regressions." So a time
   difference within a metric's measured spread (the interleaved rounds' range) is neither a gain nor a regression: it
   is noise, and ignored. Each metric's noise is defined by stage 0's baselines (the round-to-round spread of master's
   rounds); allocation counts are exact, so they're unaffected. Each stage's verdict states, per metric, whether its
   difference is beyond the spread (a real gain or regression) or within it (ignored). How noise is told from a
   difference (the user's choice, after stage 1, 2026-11-01): a Mann-Whitney U test on each metric's interleaved rounds,
   before against after; p < 0.05 is a gain or a regression, by the medians' direction; with p from 0.05 to 0.2, rounds
   are added (up to about 10 a side) and the test is run again; otherwise it's noise. Each metric reports its medians,
   ranges and p. As a stage tests many metrics (stage 1: about 115, so about 6 at p < 0.05 by chance alone), the user
   chose (after stage 1, 2026-11-01): gains count only on the stage's target cases, named in its Stages entry before
   it's measured (others' gains are reported, not claimed); any metric reaching p < 0.05 the wrong way gets a
   confirmation run of 10 fresh interleaved rounds a side, and is a regression only if that run reproduces it.
3. **The adapter's shape.** (a) One generic operation template in `coro/`; (b) hand-written operations per class;
   (c) ready tasks only where nothing waits. **Decided: (a).**
4. **Spawning without wrapper coroutines.** (a) The task's promise is the detached task (Seastar's way), in stage 5;
   (b) keep `start_on` and `run`. **Decided: (a)**, falling back to (b) if it complicates `async_scope`'s lease and
   join rules.
5. **The socket cases' proof.** (a) CPU time per unit in the benches; (b) `bench/web`'s throughput; (c) both.
   **Decided: (c).**
6. **The allocation tracer.** (a) A tool kept outside the repository, run by hand when a count surprises; (b) a
   bench-only opt-in in the repository; (c) none. **Decided: (a).**
7. **GCC.** (a) Count the cases' allocations on GCC in a throwaway Linux container at stage 0 and after stage 5; (b)
   leave GCC to CI. **Decided: (a).**
8. **The frame allocator's form, if stage 6 adopts one.** (a) Per-thread size-class free lists in the promise's
   `operator new`/`delete`, out of line; (b) a thread-local current resource (Capy's); (c) `execution::task`'s shape
   (an `allocator_arg_t` overload); (d) compare a general allocator first. **Decided: (a)**, asked again at stage 6
   with its data.
9. **`[[clang::coro_await_elidable]]`.** (a) Try it once, measured; (b) skip it (Clang-only, a hint, same translation
   unit only, and it grows the frames that are kept). **Decided: (b).**
10. **C++26.** (a) Keep the hand-rolled `task`, with operations shaped so they could be wrapped as senders, and revisit
    when libc++ ships P2300; (b) adopt stdexec now; (c) ignore it. **Decided: (a).**
11. **The allocations that aren't frames.** (a) A separate plan later; (b) folded into these stages; (c) leave them.
    **Decided: (a).**
12. **The client's request chain** (`send()` 9.9 KB, `send_http11()` 10.5 KB or `send_http2()` 11.1 KB,
    `http2_client_connection::send()` 5.3 KB, `wait_for_head()` 2 KB: 3-4 frames, ~27 KB per request, in two
    translation units and a retry loop). (a) Fold them into fewer coroutines; (b) leave them for now. **Decided: (b)**,
    revisited after stage 6.
