# Parked work

Status: a list, kept current (started 2026-11-01). Each item waits for the user's go-ahead: when the agreed work runs
out, sort these, recommend one with reasons, and ask before starting it. Items are added as the user parks them, and
taken off as they're started (moved into a plan of their own) or dropped.

## Protocols and features

- **Compression**: WebSocket permessage-deflate (RFC 7692; Autobahn's sections 12 and 13 are left out until then) and
  HTTP `Content-Encoding` (gzip, deflate, br) for request and response bodies, client and server, as one item, since
  they'd share a deflate implementation (the user, 2026-11-01). The WebSockets plan's decided question 15 left it out,
  with the options: a hand-rolled deflate (inflate first, then fixed blocks, then LZ77 matching), or zlib as an
  optional vcpkg feature, as `tls` brings Botan. Either costs memory per connection (a 32 KiB window each way with
  context takeover), and has CRIME-like risks.
- **HTTP/3** (`http3.md`).
- **The DNS resolver's cache and transport hook** ("later" in `dns-resolver.md`).
- **A regular expression engine** (`pattern.md`), perhaps as a project of its own.
- **A `tls::alert` condition for `no_renegotiation`**: a renegotiation refused fails with `sent_alert_base` + 100,
  which no condition matches; adding one changes the public API.

## Performance

- **The TLS flush's fast path**: `tls::stream`'s flush costs an allocation a batch (the ciphertext's vector; its
  coroutines went with stage 5 of the coroutines plan, which made it the stream's flusher) even when the socket takes it
  all, and its task is the flusher's even then; a ready task when no flush is in progress and the socket takes
  everything would make smaller batches cheaper still, but would take the flush on when the write's called, not when
  it's awaited (a behaviour to decide).
- **TLS encryption throughput**, with `tls::detail::connection`'s sensitivity to layout: `write_batch` among the flush's
  members made `bench_tls writes` 15% slower, so the members before it are kept where they were (what's added goes
  after it).
- **Writes that wait, as an operation**: a stream's waiting write as a `coro::operation` would halve the three or so
  allocations left over HTTP/1.1.
- **An adaptive HTTP/2 window**, sized from a measured bandwidth-delay product, as .NET's and hyper's optional one.
- **Coalescing a WebSocket writer's small messages**: a writer leaving its flush to one already waiting for the wire,
  to win back what TCP_NODELAY cost 16-byte messages in flight (11%).
- **Folding `blocking_call` back into `coro::task`**: with `~task()` always inlined, a virtual `dispose()` keeps every
  elision (probed), so `operation_task` could own heap state, and `run_blocking` return a plain task with one
  allocation.
- **Revisiting `max_next_runs`** (the thread pool's slot runs in a row, 2).
- **An allocator experiment**: jemalloc, mimalloc, snmalloc and gperftools' tcmalloc (all in vcpkg; Google's tcmalloc
  builds only with Bazel). cppnet itself stays allocator-agnostic; whether it's a benchmarks-only CMake option or
  preloading is open. Best run once allocations and sizes settle.
- **Code layout: build flags and attributes** (the user, 2026-11-13, after #120's two slowdowns from 2,431 functions
  moving 48 bytes): first decide the goal, results that don't swing with placement or faster placement, then test on
  each machine's CPU, as the effects differ. Candidates:
  - `-falign-functions=64`: a function's size no longer moves the others; it costs padding, about 370 KB for ~11,600
    functions.
  - `-falign-loops=64` for cppnet's own loops, as for Botan's (this Mac's i9-9980HK has the JCC erratum).
  - `-mbranches-within-32B-boundaries`: ELF only, so Linux on an affected Intel CPU.
  - `[[gnu::hot]]`/`[[gnu::cold]]`, or `[[gnu::aligned(64)]]` on chosen functions.
  - PGO with function ordering, or BOLT (Linux).

  `-ffunction-sections`/`-fdata-sections` with `--gc-sections` shrink the binary rather than fix placement. Mach-O already
  splits code per function and has `-dead_strip`; on ELF they are what a linker ordering file needs. MSVC's `/Gy` and
  `/OPT:REF` don't apply while there's no Windows build. Measure with the coroutines plan's bar, plus code size, and
  `perf`'s front-end counters on Linux to tell the decoded-µop cache from the erratum.
- **The web benchmark against newer suites**: TechEmpower's benchmark may not have been updated since 2024 (the user's
  report, not yet checked). HTTPArena and similar projects serve the same purpose: check what they measure differently
  (tests, load generators, rules), then propose changes to `bench/web`.

## Tests

- **A deterministic test mode**: one worker and fake time, as Go's `testing/synctest` or Tokio's paused clock.
- **`test/socket.cpp`'s "wait_writable waits until the peer has read"** failed once (2026-11-13) on Linux under
  deliberately heavy load (a full `ctest -j4` beside four stress loops of another test, in a container), while the
  DNS closed-port flake was being reproduced; its output wasn't kept, and it hasn't failed on CI. Stress it under load
  to tell a race from overload.

## Upstream

- **Botan's session ticket fix**: Botan 3.12's and 3.13's `Session_Manager_In_Memory::remove()` never finds a TLS 1.3
  client ticket over 32 bytes (stored as an `Opaque_Session_Handle`), so tickets were reused, and `find_some()` orders by
  random ID. Botan's PR #5847 (commit 33a901803c, merged 2026-08-15, after 3.13.0) fixes it; `client_sessions` in
  `src/tls/tls.cpp` works around it meanwhile. The user chose not to report it, as it's fixed upstream. Once a release
  with the fix is in vcpkg's baseline (or the overlay port), drop the workaround, keeping its test that each ticket is
  offered once.
