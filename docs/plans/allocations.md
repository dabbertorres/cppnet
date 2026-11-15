# Plan: the allocations that aren't coroutine frames

Status: surveyed (2026-11-14), questions open (see Open questions). Comes before the coroutines plan's stage 6 (the
frame allocator), and is followed by the frames only GCC makes, as the user chose (2026-11-14; `coroutines.md`'s
Decided question 11).

## Goal

The coroutines plan removed the frames on the hot paths, structurally. What each benchmark case still allocates per
unit, beyond the frames left on purpose, is the subject here: the same kind of removal (an object kept by a long-lived
owner, a lookup that doesn't build its key, a list that doesn't need a node), measured the same way. Allocations on
either compiler come first; those that only libstdc++'s shorter inline strings make are counted on GCC, in the
throwaway container the coroutines plan used.

## Where things stand

Counted on master f5362e4 (2026-11-14), Clang 23 with libc++, the `dev-clang` Release flags plus line tables, no
timing. A tracer replaced each bench's allocation counter with one that opens a window per reading, and recorded each
allocation's size and stack in it; a promise's `operator new`, made out of line in the survey's copy only, told frames
from the rest. Every traced count equals the bench's own and the coroutines plan's Clang counts after stage 5. The
tracer stays in the scratchpad, as the coroutines plan's did. libstdc++'s counts are reasoned from its 15 inline
characters against libc++'s 22, and agree with GCC's trace after stage 5.

### Frames and the rest, per case

| case | unit | total | frames | not frames |
|---|---|--:|--:|--:|
| `bench_request` pipelined / one per read | request | 1.19 / 2.19 | 1 / 2 | 0.19 (3 per batch: the connection's first request) |
| `bench_request` over a socket / handler that waits once | request | 1.0 / 1.0 | 1 / 1 | 0 |
| `bench_request` a connection per request | connection | 11 | 4 | 7 |
| `bench_request` HTTP/2 a stream at a time | request | 35 | 7 | 28 |
| `bench_request` HTTP/2 8 streams / 8 request bodies | response / request | 39.3-40.4 / 40.7 | 7.1 | 32.2-33.2 / 33.6 |
| `bench_request` with `http::timeout`, pipelined / over a socket | request | 2.19 / 2.0 | 1 | 1.19 / 1.0 |
| `bench_client` small (every reader size) | request | 16 | 5 | 11 (1 the bench's) |
| `bench_client` long head lines, 1 / 4 / 16 KiB reader | request | 51 / 32 / 25 | 21 / 10 / 5 | 30 / 22 / 20 (1 the bench's) |
| `bench_client` chunked download, 1 / 16 KiB reader | 8 MiB | 147.8 / 145.8 | 138.8 / 136.8 | 9 (1 the bench's) |
| `bench_tls echo` 16 B / 16 KiB | round trip | 14 | 0 | 14 (2 cppnet's, 12 Botan's) |
| `bench_tls writes` 16 KiB / 64 KiB / 1 MiB | write | 6.96 / 18.84 / 302.4 | 0 / 0 / 1 | cppnet's 1 / 1 / 16, the rest Botan's |
| `bench_tls` TCP alone | connection | 9.25 | 4 | 5.25 |
| `bench_tls` ECDSA full / resumed handshake | connection | 1808 / 1145 | 13 | about 97% Botan's, about 40 cppnet's |
| `bench_notifier` notifier / `wait_on` | hand-off | 1 / 3 | 1 (the bench's) | 0 / 2 |
| `bench_router` none / router / static | request | 1.3 / 2.3 / 1.3 | 1 / 2 / 1 | 0.3 (the bench's path) |

The HTTP/1.1 server allocates only frames per request once a connection has served its first; so do the WebSockets,
the routers, `bench_blocking` and `bench_task`. `bench/web` runs `bench_request`'s socket path (by reading its code, not
traced): 7 per connection, and its JSON handler's own string.

### The sites

Counts per unit on Clang with libc++. Only string lengths depend on the standard library here: vector growth, map
nodes, `std::stop_source` and `make_shared` count the same on both.

HTTP/2, 28 per stream a stream at a time (5+3+2+4+4+2+2+2+2+1+1):

- **H1**, the server's `submit_head` (`http2_server.cpp`): `stream.fields` growing (40, 80, 160, 320 B) and
  `append_fields`' `names.reserve` (96 B), 5 per stream.
- **H2**, the client's `open` (`http2_client.cpp`): `stream->fields` growing, 3 per stream (5 with a body).
- **H3**, `session::stream_for_block` and `open_stream` (`http2_session.cpp`): an `unordered_map<stream_id,
  stream_state>` node (80 B), 2 per stream.
- **H4**, `add_stream` (`http2_connection.cpp`) and `streams.emplace` (server and client): three more map nodes (32-40
  B), 4 per stream.
- **H5**, the stream objects: `http2_stream` (2808 B) and its `stream_reader` (128 B) on the server; the client's
  `make_shared<http2_client_stream>` (1336 B) and `http2_response_body` (472 B); 4 per stream.
- **H6**, `scheduler::suspend_other`, through `timer_loop`'s `wait_on()`: a `watched` multimap node (48 B), 1 per wait
  that suspends (2-2.6 per stream).
- **H7**, `scheduler::notify`: the `completions` vector (8 B), 1 per notify that wakes something (2-2.6 per stream).
- **H8**, `session::on_headers`: a copy of the header block fragment (9-18 B), 2 per stream, even when END_HEADERS is
  set and nothing more will be appended.
- **H9**, Date: the server's `resp.headers.set` and the client's `headers.add`, a 29-character value (over both
  libraries' inline sizes), 2 per stream.
- **H10**, a `std::stop_source` per stream (24 B), 1 per stream.
- **H11**, `promote` / `to_start.swap(started)` (`http2_server.cpp`): `started` growing again, 0.7-1 per stream.
- **H12**, `chunk_pool::take`: 16 KiB receive chunks, 1.3-2.1 per response with 8 streams (it varies with timing).
- **H13**, the client's `send_body`: a 16 KiB body buffer, 1 per request with a body.

The HTTP/1.1 client, 10 per request (up to 29 with long head lines; the bench's own URL copy aside):

- **C1**, `basic_headers::get_list` (`headers.cpp`), through `get_content_length` and the other list readers: a
  one-element `vector<string_view>`, 4 per request (2 when chunked).
- **C2**, `pooled_body` (`client.cpp`): `make_unique<buffered_reader>` (232 B) and its buffer (1-16 KiB), 2 per
  request.
- **C3**, `send_http11`: the `buffered_writer`'s 4 KiB buffer, 1 per request.
- **C4**, `make_unique<pooled_body>` (392 B), 1 per request.
- **C5**, `make_response_body` (`http11.cpp`): the `limit_reader` or `chunked_reader` (120 B), 1 per request.
- **C6**, `basic_headers::add` from `add_field_line`: Date (32 B), 1 per request; with long head lines, the cookie
  values (1552 B) and the values' list spilling, 10. Values of 16-22 characters allocate on libstdc++ only.
- **C7**, `io::read_line` (`util.cpp`): the line's string growing when a line spans the reader's buffer, 10 / 2 / 0 per
  request with long head lines and a 1 / 4 / 16 KiB reader.
- **C8**, `connection_pool::make_key` and the lease's copy of the key: 0 per request on libc++ here, 3 on libstdc++.
  The benches' key, `http://127.0.0.1 54321`, is exactly libc++'s 22 characters; an `https` host of 11 characters or
  more on port 443 passes it on libc++ too.

Waits, TLS and connections:

- **S1, S2**: H6 and H7 outside HTTP/2: `bench_notifier`'s `wait_on` row allocates 2 per hand-off.
- **T1**, `tls_emit_data` (`tls.cpp`): `flush_done` (and `flush_canceled`) free `flush_pending`, so the next flush's
  ciphertext vector starts empty: 1 per flush that writes a record (2 per echo round trip, 16 per 1 MiB write).
- **T2**, Botan's record layer (`Record_Layer::prepare_records`, `next_record`, `Channel_Impl_13`): record headers and
  record and plaintext copies, 12 per echo round trip and about 6 per 16 KiB write.
- **P1**, the server's `connection_state`: two 16 KiB buffers per connection.
- **P2**, `register_handle` (`scheduler.cpp`): a `registrations` map node (56 B) per socket.
- **P3**, a `std::stop_source` per HTTP/1.1 connection; **P6**, one per request under `http::timeout`
  (`linked_stop_source`).
- **P4**, a connection's first request: the Accept value (96 B), Date (32 B) and the body reader (120 B).
- **P5**, `connect_resolving` (`tcp.cpp`) and the resolver's `at_once` / `look`: small vectors, 3 per client connection.

## Proposed stages

One pull request each, each safe on its own, with its target cases named before measuring. All are on either compiler
but the pool's key, which shows on libc++ only with a case whose key is long.

0. **Benches.** `bench_client`'s `get_all` takes a `string_view` (1 fewer per request on every case, re-baselined); a
   case whose pool key is longer than 22 characters (Open question 11); baselines on master.
1. **Waits without map nodes** (S1, S2). Targets: `bench_notifier`'s `wait_on` rows (3 -> 1 per hand-off; time);
   `bench_request`'s HTTP/2 cases (a stream at a time 35 -> 31, 8 streams about 5 fewer per response, 8 request bodies
   about 4.8 fewer; time and CPU time).
2. **HTTP/2 header blocks** (H1, H2, H8, H11). Targets: a stream at a time 31 -> 20; 8 streams about 9.8 fewer per
   response; 8 request bodies about 11.7 fewer.
3. **HTTP/2 stream maps** (H3, H4). Targets: 6 fewer per stream on the three HTTP/2 cases.
4. **HTTP/2 stream objects, and Date written when encoding** (H5, the server's H9, P4's Date). Targets: 5 fewer per
   stream on the HTTP/2 cases; 1 fewer per connection (pipelined 1.19 -> 1.13, a connection per request).
5. **The HTTP/1.1 client's objects per request** (C1-C5), probably two pull requests: 5a (C1-C3: small 16 -> 9, long
   head lines 7 fewer, chunked 5 fewer, and 2 fewer on `bench_request`'s 8 request bodies, from C1), 5b (C4, C5: small
   -> 7, 2 fewer on the others). Targets: every `bench_client` case and reader size.
6. **The pool's key** (C8). Targets: stage 0's long-key case on libc++; on GCC, 3 fewer on every `bench_client` case,
   counted in the throwaway container.
7. **TLS's ciphertext buffers** (T1). Targets: echo 14 -> 12 per round trip; writes 16 KiB 6.96 -> 5.96, 64 KiB 18.84
   -> 17.84, 1 MiB about 302 -> 286; the handshakes, about 7 fewer per connection. The peak memory the bench reports
   stays within the retention limit.
8. **Per connection** (P1, P2, P5): a connection per request 11 -> about 8, TCP alone 9.25 -> about 5. Proposed to be
   parked (Open question 12).

Left out unless a question says otherwise: stop sources (H10, P3, P6), the client's header values (C6), `read_line`
(C7), the receive-chunk pool (H12), the body buffer (H13), and Botan's allocations (T2).

## Measuring

As in the coroutines plan, unless Open question 1 decides otherwise: allocation counts are exact; time and CPU time
by Mann-Whitney U on 10 interleaved rounds a side, p < 0.05; gains count only on the target cases named before
measuring; a wrong-way result gets a confirmation run of 10 more rounds, and a confirmed regression goes to the user
with its evidence.

## Open questions

1. **The bar.** (a) The coroutines plan's (any allocation drop or gain keeps a change; with neither, nor a regression,
   readability decides); (b) a different one. Recommended: (a).
2. **The order and split.** (a) As proposed above (stages 0-8); (b) changed. Recommended: (a).
3. **`wait_on()`'s structure** (stage 1). (a) Waiters linked intrusively (they already live in the operation) into a
   fixed hash table of first-in-first-out lists; (b) keep the multimap, with spare nodes; `notify_one` resumes its one
   handle without a vector, `notify_all` collects into a small inline buffer either way. Recommended: (a), falling back
   to (b) if its locking or ordering rules get hard to keep.
4. **Maps keyed by stream id or fd** (H3, H4, P2). (a) Reuse nodes through a small `util` wrapper; (b) a hand-rolled
   open-addressing table keyed by integer; (c) fold the session's state and the active set into the stream object.
   Recommended: (a).
5. **Recycling HTTP/2 stream objects** (stage 4). (a) A free list per connection (the client's `allocate_shared` with
   a per-connection allocator, if the connection outlives the bodies holding its streams; not yet checked); (b) a
   per-thread size-class pool, shared with stage 6's frame allocator if it comes; (c) leave them. Recommended: (a),
   the server first.
6. **The client's objects per request** (stage 5). (a) The pooled connection owns the buffers, and a body slot that
   comes back when the response's body is destroyed; (b) a free list per type, through class `operator new`; (c) the
   buffers only. Recommended: (a) for the buffers and body reader, then whichever of (a) or (b) keeps
   `client_response::body` a plain `unique_ptr<io::reader>`.
7. **Header values** (C6, the client's H9). (a) Keep a field's value strings across `clear()`, with Date written when
   encoding; (b) one buffer per message for all names and values (one allocation per message on both libraries, but a
   rewrite of `basic_headers`); (c) leave them. Recommended: (a); (b) only if long fields become target cases.
8. **Stop sources** (H10, P3, P6). (a) A hand-rolled in-place stop source, shaped like C++26's
   `inplace_stop_source`, replacing `std::stop_token` in the API; (b) keep `std::stop_token`, with fewer sources (e.g.
   `http::timeout` without a linked one); (c) leave them, revisited with libc++'s P2300 (the coroutines plan's Decided
   question 10). Recommended: (c), as (a) changes every public signature that takes a token.
9. **TLS's buffers** (T1; they're freed on purpose today). (a) Keep both buffers' capacity, up to a write batch plus
   the records' overhead; (b) keep it without a limit; (c) a pool shared across connections. Recommended: (a).
10. **Botan's allocations** (T2: 12 of 14 per echo round trip, about 97% of a handshake's). (a) Leave them, recorded as
    `bench_tls`'s floor; (b) patch the record layer in `ports/botan/`; (c) propose it upstream. Recommended: (a).
11. **A case for the pool's key on libc++** (stage 0). (a) A `bench_client` case whose key passes 22 characters (e.g.
    `localhost.`, if the resolver takes the trailing dot; not yet checked), with a long path and query; (b) count C8 on
    GCC only. Recommended: (a).
12. **Allocations per connection** (stage 8). (a) Park them; (b) a stage as proposed. Recommended: (a).
13. **What's left out** (C7 `read_line`, H12 the receive-chunk pool, H13 the body buffer). (a) Leave them; (b) a stage
    for each. Recommended: (a) for H12 and H13; C7 with GCC's extra frames, as `read_line` is one of them.
