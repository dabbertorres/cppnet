# CLAUDE.md

## Overview

`net` is a C++23 networking library (TCP/UDP sockets, DNS, URL parsing, HTTP/1.1 and HTTP/2 client and server, Prometheus/OpenTelemetry instrumentation). It is built on its own coroutine runtime. The only external dependencies are spdlog (public), glaze (private: JSON, for OTLP), Botan (private, optional: TLS) and Catch2 (tests). They come from vcpkg in manifest mode (`vcpkg.json`).

User-facing docs are `README.md` (building, using cppnet from another CMake project, and working on it) and `docs/guide.md` (the runtime, errors, lifetimes, each module, and which headers are public): keep them in step with the API (the guide's snippets were compile-checked as a throwaway example). `docs/plans/` holds design plans.

## Building, testing and tooling

`README.md`'s "Building" and "Development" sections have the presets and build directories, the Botan port, the commands for tests, stress runs, fuzzing and benchmarks, the formatting and lint tools (pinned versions, IWYU), and what CI checks: read them before building, testing, linting, benchmarking or touching `ports/`, and keep them current.

```sh
cmake --preset dev-clang && cmake --build build --config Debug && ctest --test-dir build -C Debug
```

## Writing tests

- Test files mirror the `src/` layout, for example `test/util/uuid.cpp`. Tags follow `[module][submodule][aspect]`, for example `[util][uuid][version]`. Table-driven tests use `GENERATE(test_case{...})` with designated initializers.
- Don't `co_await` inside Catch2's assertion macros (`REQUIRE(co_await x)`, `CHECK(...)`): GCC evaluates a `co_await` in the macro's never-run loop condition too, so `x` runs twice (e.g. a read consumes twice as much). Await into a local, then assert on it. The same goes for a `co_await` in one operand of `||`/`&&` when the other makes a temporary with a destructor: GCC 16 got `if (!co_await wait() || header(...) != "yes")` wrong (true with both operands false).
- Tests over real sockets and the scheduler's workers run their tasks on several threads, so an ordering a test relies on across tasks (the handler has seen the reset, the server has read the request, a helper's tasks are done with it) must be waited for, on a signal the other side sets (a word with `wait_on()`/`notify_all()`, an `async_scope`'s `join()`), never assumed to have happened by now. Each race the tests have had was one such assumption (in about 1 run in 200, so one run passes and CI fails later). Stress a new or changed concurrent test once its code has settled, not after every change (the README has the command).
- A failing `REQUIRE` unwinds the test, destroying what's on its stack in reverse order: declare what the scheduler's tasks use after they could be ended some other way (e.g. a flag that a server's `serve()` task sets once it finishes) before the scheduler, whose destruction waits for them. Catch2 also runs a test case with sections once more after a section fails, to look for more, so the code outside the sections runs (e.g. spawning `serve()`) without any of them.
- With `test/dns/blocking_lookup.hpp`, `run_to_completion()` waits for every blocked call, so a test that leaves a family's call blocked releases it before its task returns (a `util::defer` in the test's coroutine); a test that needs a family's answer in before going on joins the lookup with `start_resolving()` and waits on its `news()`, rather than sleeping.

## Working on cppnet

- cppnet is a learning project, meant to be usable for side projects but not for production. Hand-rolling the runtime, parsers and protocols is the point, so don't propose libraries in their place (nor `std::regex`: the regex plan, `docs/plans/pattern.md`, is a hand-rolled Thompson NFA, with RE2 and CTRE as design references only). Park a half-built subsystem rather than leave it half-done in the tree, and recommend a reverse proxy in front of anything public.
- Git follows the user's global workflow (a branch per change, pull requests only with the user's approval, master fast-forwarded once CI passes), with cppnet's specifics: the check to wait for is `build`; master's protection also asks for an approving review, which the sole collaborator can't give, so the fast-forward push bypasses it as an admin (GitHub prints "Bypassed rule violations"); CI runs only for pull requests into master, so a stacked pull request gets CI once its base is merged and it's retargeted (fast-forwarding keeps hashes, so it needs no rebase); plan documents (`docs/plans/`) may go to master directly, still as a plain fast-forward, which then means rebasing an open pull request.
- Decisions are recorded in the plan they belong to (`docs/plans/`, in its decided questions or its stage's notes), not left in a pull request or a conversation (the first reviews' choices, from before there was a plan per area, are in `docs/plans/early-decisions.md`); work the user parks goes in `docs/plans/parked.md`, which says what's waiting for the user's go-ahead.
- CI does the full verification (ThreadSanitizer, the build without TLS, the whole suite on four toolchains), and its clang-tidy fails on warnings in any file a change touches, so warnings already in a file get fixed when a change touches it. Locally, build and run the affected tests and the suite once (with ASan/UBSan, as the presets build), and leave TSan, the no-TLS build and repeated runs to CI, fixing what it finds with fixups.
- The repo was formatted all at once (`db31961`); keep it so (format whole files you create, and only your changed lines in files that weren't clean, though all are now), with the pinned clang-format (README).
- Implementation is usually delegated to agents. Their briefs carry this file's rules that are easy to miss: measuring before and after, explicit waits in tests, no test hooks, the terms below. The main session reviews their work, pushes, opens pull requests and merges.
- Code conventions:
  - Failures return `std::expected<T, std::error_code>` (see the errors paragraph under Architecture); `std::optional` is for values that are legitimately absent. A new error enum is an `is_error_code_enum`, with a category whose `default_error_condition()`/`equivalent()` map it to conditions.
  - Exclusive locks are `std::scoped_lock` (not `std::lock_guard`), shared ones `std::shared_lock`.
  - Code run at scope exit is a `util::defer` (with a `noexcept` callable), not an ad-hoc RAII struct.
  - Arguments that must not be null are references, in code written or touched (no sweep of existing code). Stored pointers such as `io::scheduler*` stay pointers (a reference member makes the type non-assignable), and so do results that a `std::expected` or `std::optional` holds. A coroutine that runs detached, or outlives its caller's frame, takes neither references nor pointers to the caller's locals.
  - Running out of memory is fatal: `std::bad_alloc` isn't caught, and clang-tidy's `bugprone-exception-escape` ignores it, `bad_array_new_length` and `length_error` (its `IgnoredExceptions` list is comma-separated; with `;` it silently ignores nothing).
  - Aggregates' members have default initializers (`{}`), so designated initializers name only what they set.
  - `util::string_map` exists for heterogeneous lookup: extend it rather than work around it.
  - A peer must never control the server: a busy peer, not just an idle one, mustn't keep `close()`/`join()` or a resource bound from finishing. Enforce deadlines by the clock, not only in waits; a small fixed cost per read for that is acceptable.
- Tests:
  - No test hooks in library code (test-only accessors, parameters, friends) without a very good reason: tests observe behaviour, through the public API or the other side (e.g. what the server sees). Ask the user before adding one. Saving real-time waits was reason enough once (`tls::detail::verify_for()`'s time, so no test sleeps past a certificate's expiry). Every path gets a test, not just an argument that it works.
  - Retries in tests are the exception. For an ordering race, remove it at its source, in the library (the server listening in its constructor fixed a connect racing `serve()`) or in the test's structure; retry only where nothing better exists (an external process, the OS), and say why there.
  - A negative check (nothing sent, no reset, the peer hasn't closed) starts its timed wait only after an explicit signal that the thing would already have happened; an ordered proof (e.g. a PING round trip) is better than a timer where there is one. Catch2 isn't thread-safe: tasks on workers record, the test's thread asserts. When a wait for tasks that point into a frame times out, abort what they block on, then wait for them without a deadline.
- Measuring:
  - Measure by default whenever a change may affect performance (a hot path, buffer sizes, windows, copies, allocations, task hand-offs): run the relevant `bench/` target before and after (a new case if none covers it) and give the numbers in the pull request. When measuring it accurately needs a piece that doesn't exist yet (e.g. the client as well as the server), it waits until both do, and is measured then: say so in the pull request meanwhile.
  - Use standard terms in plans, pull requests and reports: a benchmark case (a scenario a bench measures), a metric (time, CPU time, allocations per unit), target cases (those a change is expected to improve, named before measuring).
  - The coroutines plan (`docs/plans/coroutines.md`) sets how a change's results are judged (its keep/drop bar and statistics).
- Parallel work runs in separate git worktrees, each with its own build directory (`cmake --preset ...` in each; vcpkg's binary cache makes that quick), never in another agent's checkout. Benchmarks and stress runs don't share the machine with them.

## Architecture

Everything lives in `namespace net`, with sub-namespaces that match the directories (`net::io`, `net::coro`, `net::http`, `net::util`, ...). Public headers are in `include/<area>/`. Implementations are in the matching path under `src/`. Headers are included relative to `include/`, for example `#include "io/scheduler.hpp"`.

Each area's design, decisions and status are in its plan (`docs/plans/`), its use in `docs/guide.md`, and its internals in the headers' and sources' comments (the `NOTE`s say why). Read them before changing an area, and keep them current rather than describing the area here.

- **Coroutines (`coro/`)**: `coro::task<T>` (lazy; awaiting resumes it directly, so `co_await`s that finish without suspending don't grow the stack), `ready_task()` for results without a coroutine, `coro::operation`/`adapter`/`stepper` for waits without a coroutine frame, `thread_pool` (its idle workers poll for I/O), `notifier`, `async_scope`. Plan: `coroutines.md`.
- **Scheduler (`io/`)**: `io::scheduler` ties the event loop (`epoll_loop` on Linux, `kqueue_loop` on macOS/BSD; IOCP has only its macro, `NET_HAS_COMPLETION_PORTS`), the pool and an `async_scope` together, and owns waits and their deadlines, futex-style `wait_on()`/`notify_*()`, `call_at()` and stop tokens per wait. Plans: `handler-cancellation.md`, `middleware.md` (stage 4, `call_at()`).
- **Sockets and streams (`socket.hpp`, `tcp.hpp`, `udp.hpp`, `listen.hpp`, `io/`)**: non-blocking sockets with deadlines, timeouts and stop tokens; `io::reader`/`io::writer` and their wrappers (buffered, limited); `tcp_socket::connect()` resolves and connects with Happy Eyeballs.
- **Errors**: `std::expected<T, std::error_code>`, with the library's own error enums compared against conditions (guide's "Errors"; `early-decisions.md`).
- **HTTP (`http/`)**: the server (HTTP/1.1, and HTTP/2 over cleartext or ALPN; tunnels for upgrades), the client (connection pool, HTTP/2, redirects), routers and middleware (`http::chain`, `http::timeout`), HPACK and Structured Fields. HTTP/1.1 decoding (`http11.cpp`) is deliberately strict, as request smuggling exploits ambiguity between the server and a proxy. Byte-wise parsing reads from `buffered_reader::buffered()`/`consume()` rather than `co_await`ing each byte. Plans: `http2.md`, `middleware.md`, `client-redirects.md`, `handler-cancellation.md`; HTTP/3 is parked in `http3.md`.
- **WebSockets (`websocket/`)**: client and server, over HTTP/1.1 and HTTP/2 (extended CONNECT). Plan: `websockets.md`.
- **DNS (`dns/`)**: the resolver (system and native back ends; the default picks per name), its errors, and the message codec. Plan: `dns-resolver.md`.
- **TLS (`tls/`, with `NET_HAS_TLS`)**: `tls::stream` over a `tcp_socket`, with Botan kept private. Plan: `tls.md`.
- **Instrumentation (`instrument/`)**: Prometheus metrics, OpenTelemetry trace context and OTLP/JSON (glaze, private), and `http_metrics`/`http_tracing` middleware; spans go to a callback, as there's no exporter yet.
- **The rest**: `url`, `ip_addr`/`ipv4_addr`/`ipv6_addr`, `encoding/` (base64, hex), `util/` (small self-contained utilities).

`example/http-server/main.cpp` shows a typical program's setup (signals blocked before threads start, the scheduler, `server.close()` then `join()` on a signal, then `scheduler.shutdown()`); `run_to_completion(task)` suits a program that runs one task (`example/http-get`).

### Invariants and gotchas

- Layering: `encoding` → `util` → `coro` → `io`. `util/` depends only on `encoding/` and `config.hpp`'s platform macros; keep it that way, so the layers don't form cycles.
- Never request stop while holding a scheduler shard's lock, `timers_mu` or an HTTP/2 connection's `mu`: stop callbacks take them (`connection_mutex` checks it in debug builds). Lock order: a shard's lock, then `timers_mu`.
- Coroutine frames: Clang elides one (HALO) only for a coroutine called directly in a `co_await`, outside a loop, never through a wrapper that isn't a coroutine or through an indirect call (`coroutines.md`, "Which calls Clang elides"). Keep the awaiter holding the handle rather than the task, keep `task`'s destructor `[[gnu::always_inline]]`, and make a coroutine whose frame must be its own `[[gnu::noinline]]` (`task.hpp`'s notes). A function that can often finish without waiting returns a `ready_task`, and calls a coroutine only when it must wait.
- Botan's endpoints agree on TLS 1.3 unless one is limited, so a TLS 1.2 test limits one side.
- A Botan module cppnet starts using goes in the portfile's module list (`configure.py` can't see algorithms looked up by name at runtime); `./tests "[tls][policy]"` fails if one is missing.
