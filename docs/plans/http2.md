# Plan: HTTP/2

Status: proposed, with its design decided (2026-10-17). Nothing is implemented: `include/http/http2.hpp` and
`src/http/http2.cpp` are a parked stub, to be replaced (see Where things stand).

## Goal

HTTP/2 in both `http::server` and `http::client`, hand-rolled as HTTP/1.1 is (framing, HPACK, flow control), and on by
default: over TLS (ALPN `h2`), and over cleartext TCP with prior knowledge. Handlers and callers shouldn't need to know
which version a request came over: `request_handler`, `response_writer` and `client::send()` stay as they are. The
connection's machinery (streams, the handlers' tasks, `response_writer`'s back ends) is shaped so that HTTP/3
([http3.md](http3.md), parked), whose streams QUIC provides, can reuse it later.

## Decided

- **Hand-rolled, on by default.** The server offers `h2` (first) and `http/1.1` with ALPN, and so does the client;
  either can be turned off (`server_config`, `client_options`).
- **Cleartext HTTP/2 is prior knowledge only.** RFC 9113 dropped `Upgrade: h2c`, but kept cleartext HTTP/2 for a
  client that starts with the preface (§3.3): it's what h2load and h2spec use without TLS, and what gRPC without TLS,
  and proxies talking to the servers behind them (nginx, Envoy, HAProxy), speak. The server peeks at a cleartext
  connection's first bytes; the client uses it only when asked.
- **Frames are read and written explicitly**, not as packed structs: multi-byte fields are loaded with `memcpy` and
  `std::byteswap` (on little-endian hosts; a single load-and-swap instruction once compiled), and the 24-bit length
  and 31-bit stream id with shifts and masks. Packing alone wouldn't match the wire: HTTP/2 is big-endian, bitfields'
  order is the compiler's (GCC and Clang fill them from the low bit on little-endian targets, so the stub's
  `length : 24` and reserved bit land in the wrong places), and reading a buffer through a struct needs `memcpy` (or
  `std::start_lifetime_as`) anyway. `util/packed.hpp`'s only user is the stub, so it goes with it.
- **One writer per connection**, draining a queue that the streams' `response_writer`s add to (as Go's server does):
  the socket takes one writer at a time, and HPACK's encoder must run in the order its blocks are sent.
- **The new primitives (an async queue, and a counter to wait on) go in `coro/`**, as they do no I/O: each keeps its
  own list of waiting coroutines, resumed on the pool, as `async_scope`'s join does. That gives each its own lock,
  where every `wait_on` and `notify` shares the scheduler's one lock (`timers_mu`), which every flow-control wait and
  queued frame, on every connection, would contend on. What it costs: no deadlines (timeouts come from the
  per-connection timer task; see Timeouts), no waking by `request_stop()` (their owner closes them when the connection
  ends), and new synchronization code to test (with ThreadSanitizer). A benchmark (`bench/coro/`) makes the reasoning
  concrete: many tasks, as streams on many connections would, passing items through queues and waiting on counters,
  with the `coro/` primitives and with the same built on `wait_on`, across worker counts; it's written with the
  primitives, before HTTP/2 uses them, so a surprise changes the plan rather than the code.
- **`response_writer`'s back end is a `std::variant`** of HTTP/1.1's and HTTP/2's (and later HTTP/3's): the set of
  versions is fixed, and the variant keeps them apart at the type level. It's the head's and framing's back end: the
  body writer handlers get stays an `io::writer*`.
- **Timeouts are per stream, as in Go, nginx and Envoy**: a stream slow to send its body, or stuck behind a window the
  peer doesn't open, is ended alone (with 408 if nothing was sent yet, otherwise RST_STREAM), and the connection's
  other streams go on. Only the connection's own waits are per connection (see Timeouts).
- **At the server's stream limit, the client opens another connection** (as Go, curl and OkHttp do), up to
  `max_connections_per_host`, and only then waits for a stream (within `connect_timeout`). A setting makes it wait on
  the one connection instead (as Chromium, hyper and gRPC do).
- **HPACK's encoder doesn't index into its dynamic table at first** (static table matches and literals): simpler, and
  the dynamic table is where compression oracles live. It may come later, opt-in per field.
- **No server push** (browsers dropped it), and RFC 7540's priority tree is parsed and ignored (RFC 9113 deprecated
  it); RFC 9218's priorities may come later.

## Specifications and references

- [RFC 9113](https://www.rfc-editor.org/rfc/rfc9113.html), HTTP/2: framing, streams, flow control, the mapping of
  HTTP's semantics, TLS. It obsoletes RFC 7540; what changed (Appendix B): the 7540 priority tree is deprecated (its
  frames must still be parsed), the `Upgrade: h2c` mechanism is gone (cleartext is prior knowledge only, §3.3), field
  validation is stricter, and Host and `:authority` mustn't disagree.
- [RFC 7541](https://www.rfc-editor.org/rfc/rfc7541.html), HPACK: header compression. Test vectors in its Appendix C,
  and [hpack-test-case](https://github.com/http2jp/hpack-test-case).
- [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html), HTTP semantics: what's shared with HTTP/1.1 (methods,
  status codes, fields, `:authority` as the origin, 421 Misdirected Request, idempotency for retries).
- [RFC 9218](https://www.rfc-editor.org/rfc/rfc9218.html), Extensible Prioritization (`Priority` header,
  `PRIORITY_UPDATE`): optional, as priority is only a suggestion. Later.
- [RFC 8441](https://www.rfc-editor.org/rfc/rfc8441.html), extended CONNECT (WebSockets over HTTP/2). Later.
- [RFC 7301](https://www.rfc-editor.org/rfc/rfc7301.html), ALPN: `h2` and `http/1.1`.
- Implementations to learn from: Go's [x/net/http2](https://pkg.go.dev/golang.org/x/net/http2) (`server.go`: a serve
  loop owning the connection, a goroutine per handler, a write scheduler; moving into the standard library's
  `net/http` as of Go 1.27), Rust's [h2](https://docs.rs/h2) (as hyper uses it: explicit send capacity, and receive
  capacity released as the application reads), and [nghttp2](https://nghttp2.org/) (a callback-driven state machine
  without I/O; its HPACK Huffman decoder is a nibble state table,
  [`nghttp2_hd_huffman.h`](https://github.com/nghttp2/nghttp2/blob/master/lib/nghttp2_hd_huffman.h)).
- Attacks every server must handle (see Limits): Rapid Reset
  ([CVE-2023-44487](https://blog.cloudflare.com/technical-breakdown-http2-rapid-reset-ddos-attack/)), MadeYouReset
  ([VU#767506](https://www.kb.cert.org/vuls/id/767506), CVE-2025-8671), CONTINUATION floods
  ([VU#421644](https://www.kb.cert.org/vuls/id/421644), 2024), the HPACK bomb
  ([CVE-2016-6581](https://github.com/python-hyper/hpack/blob/master/docs/source/security/CVE-2016-6581.rst)), and the
  2019 set ([CVE-2019-9511 to 9518](https://github.com/Netflix/security-bulletins/blob/master/advisories/third-party/2019-002.md):
  data dribble, ping, reset, settings and empty-frame floods, resource loops, internal buffering).
- Tools: [h2spec](https://github.com/summerwind/h2spec) (conformance; written for RFC 7540, last released 2020),
  [h2load](https://nghttp2.org/documentation/h2load-howto.html) and `nghttp -v` (load, and frame-level debugging),
  curl's `--http2` and `--http2-prior-knowledge`. Browsers only speak `h2` over TLS.

## Where things stand

- The stub (`src/http/http2.cpp`) has the RFC's enums, and packed bitfield structs for the frames, which don't match
  the wire (see Decided); no HPACK; its four functions return `function_not_supported`. It's replaced, not built on.
- The server serves HTTP/1.1 only: a TLS connection that agrees on another protocol with ALPN is closed
  (`server::accept_tls`); a request line of `HTTP/2.0`, or the HTTP/2 preface (`PRI * HTTP/2.0`), gets an error.
  `serve_requests` is one coroutine serving a connection's requests one after another, with one `server_request`,
  `server_response` and buffers reused for each (`connection_state`).
- `response_writer` frames bodies for HTTP/1.1 (Content-Length, chunked, or until the connection closes), through a
  function pointer (`http11::response_encode`) and members typed for HTTP/1.1 (`http11::chunked_writer`).
- The client's pool lends a connection to one request at a time (an exclusive `lease`, returned once the response's
  body is read), and offers only `http/1.1` with ALPN.
- `headers` can hold lowercase names (it compares names ignoring case), but pseudo-headers (`:path`, ...) don't
  belong in it: they map onto `server_request`'s fields.
- The runtime has what multiplexing needs underneath (`async_scope`, `scheduler::spawn`, `wait_on`/`notify` on a
  word, `sleep_until`, `buffered_reader`/`buffered_writer`), and a socket (or `tls::stream`) allows one read and one
  write at once. There's no queue, channel or async mutex yet.

## How HTTP/2 works

- **Preface.** The client sends `PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n` and a SETTINGS frame; the server's preface is a
  SETTINGS frame. Each SETTINGS frame must be acknowledged.
- **Frames.** A 9-octet header (24-bit length, type, flags, a 31-bit stream id; stream 0 is the connection), then the
  payload: DATA, HEADERS, PRIORITY (deprecated, still parsed), RST_STREAM, SETTINGS, PUSH_PROMISE, PING, GOAWAY,
  WINDOW_UPDATE, CONTINUATION. Unknown frame types and settings are ignored.
- **Settings** and their initial values: HEADER_TABLE_SIZE 4096, ENABLE_PUSH 1, MAX_CONCURRENT_STREAMS unlimited (100
  or more recommended), INITIAL_WINDOW_SIZE 65535, MAX_FRAME_SIZE 16384 (up to 16777215), MAX_HEADER_LIST_SIZE
  unlimited (advisory; a field counts its name, value and 32).
- **Streams.** Clients open odd ids, servers even ones (only for push), always increasing. A stream is idle, open,
  half-closed (one side has sent END_STREAM) or closed; RST_STREAM ends one at once.
- **Messages.** A HEADERS frame (and CONTINUATION frames, with nothing else in between, until END_HEADERS), DATA
  frames, and optionally trailers (a second HEADERS frame); END_STREAM on the last. Pseudo-headers come first:
  `:method`, `:scheme`, `:path` and `:authority` for requests (CONNECT: `:method` and `:authority`), `:status` for
  responses. Names are lowercase; Connection, Keep-Alive, Proxy-Connection, Transfer-Encoding and Upgrade are
  malformed, and TE may only be `trailers`. Content-Length must match the DATA's length. Cookie may arrive in pieces,
  to be joined with `"; "`.
- **Flow control.** Only DATA counts. Each stream has a window, and so does the connection (65535, changed only by
  WINDOW_UPDATE): a sender may send as much as the smaller of them allows. A change of INITIAL_WINDOW_SIZE moves every
  open stream's window by the difference (so it may go negative). Windows top out at 2^31-1.
- **Errors and shutdown.** A stream error resets the stream (RST_STREAM); a connection error ends the connection
  (GOAWAY, then close). GOAWAY says the last stream the sender will process: streams after it weren't, and may be
  retried elsewhere. Graceful shutdown sends GOAWAY(2^31-1), waits a round trip (a PING), then GOAWAY with the real
  last stream, and drains.
- **HPACK.** Fields are encoded against a static table (61 entries) and a dynamic table (of the peer's choosing size,
  entries costing their name and value and 32 octets, oldest evicted first), as indexes, or literals (optionally added
  to the table, or never indexed, for secrets such as Authorization and Cookie), with integers in a prefix-plus-7-bit
  scheme and strings optionally Huffman coded. A header block must be decoded even if it's then rejected, to keep the
  tables in step.
- **Push** is gone in practice (Chrome removed it in version 106, recommending 103 Early Hints): the server never
  pushes, and the client sends ENABLE_PUSH 0.

## Design

### Framing and HPACK

- **Frames** are read from the connection's `buffered_reader` (a whole frame is usually buffered, so it's decoded
  without a coroutine, as HTTP/1.1's heads are) and written into its `buffered_writer`, field by field (see Decided).
  They're plain structs (a type, flags, stream id, and a view of the payload), with no I/O, so the codec is tested and
  fuzzed on bytes.
- **HPACK** (`http/hpack.hpp`): a decoder and an encoder, each with its dynamic table (a ring of entries, with their
  sizes). The static table and the Huffman code (RFC 7541 Appendices A and B) are `constexpr`; the Huffman decoder is
  a nibble state table generated from the code at compile time, as nghttp2's (the bit-at-a-time tree walk being the
  simple, slow alternative). The Huffman code is shared with QPACK, should HTTP/3 come. The decoder caps integers
  (their value and their octets), string lengths, and the decoded header list, as it goes. The encoder emits static
  table matches and literals without indexing, Huffman coded when that's shorter, with never-indexed literals for
  sensitive fields (Authorization, Cookie, Set-Cookie, ...).

### A connection

One coroutine owns each HTTP/2 connection: it reads frames, and holds the connection's state (the streams, settings,
both HPACK tables, windows). Handlers run as tasks of their own, one per stream, so a slow handler holds up only its
stream:

- **Reading.** The connection coroutine reads frames and acts on them: SETTINGS, PING and WINDOW_UPDATE itself; a
  request's HEADERS (with its CONTINUATIONs) decoded into a `server_request`, whose handler is spawned; DATA appended
  to its stream's receive buffer, for the request's body reader. It never waits for a handler, so control frames are
  always answered (RFC 9113 §5.2.2 requires reading to go on, or flow control can deadlock).
- **Writing.** One writer task per connection drains a queue that the handlers' `response_writer`s add to (heads,
  body chunks, trailers, resets): it encodes and frames them, round-robin across streams, in chunks of at most
  min(MAX_FRAME_SIZE, the windows), with control frames (SETTINGS and PING acknowledgements, WINDOW_UPDATE, GOAWAY)
  first.
- **Flow control.** A handler's body writes wait for window, both the stream's and the connection's, which the
  connection coroutine adds to as WINDOW_UPDATEs arrive. On the receiving side, the server advertises a window per
  stream and buffers at most that much; as the handler reads the body, the read data is credited back
  (WINDOW_UPDATE), as Rust's h2 does, so a handler that doesn't read stops its client without holding up other
  streams.
- **Handlers' tasks** are owned by the connection (an `async_scope`, with the server's lease), so closing the
  connection waits for them; the number running is MAX_CONCURRENT_STREAMS (advertised; more are refused with
  REFUSED_STREAM, which a client may retry).
- **New primitives.** The writer's queue and the windows need an async queue and a counter to wait on, in `coro/`
  (see Decided). Whatever waits on them is woken when the connection ends (it closes them), and per-stream timeouts
  reset their streams, which wakes their waits too.

### Server

- **Choosing the protocol.** Over TLS, by ALPN: the server context offers `h2` and `http/1.1` (`h2` first), and
  `accept_tls` keeps both; then `serve_connection` runs `serve_http2` or `serve_requests`. Over cleartext, by prior
  knowledge: the first bytes are peeked, and the HTTP/2 preface starts HTTP/2 (an HTTP/1.1 request line can't begin
  with `PRI * HTTP/2.0`, which is why the preface was chosen).
- **Requests.** Pseudo-headers fill `server_request`'s fields (`:method`, `:path` parsed as a request target,
  `:authority` as the host, `:scheme`), and `version` is 2.0. Fields are validated by HTTP/2's rules (above): a
  malformed request is a stream error (RST_STREAM(PROTOCOL_ERROR)), or a 400. `server_request`s are allocated per
  stream, and reused from a free list per connection, as HTTP/1.1 reuses its one.
- **`response_writer`** holds its back end as a variant (see Decided): HTTP/1.1's (what it does now) or HTTP/2's (a
  HEADERS frame; a body writer framing DATA under flow control; END_STREAM on `finish()`). Its rules carry over: 1xx
  responses (HEADERS without END_STREAM), none for 204 and 304, Content-Length limiting the body, and `send(status)`
  for a body of unknown length (which HTTP/2 frames without anything special). Connection-specific fields a handler
  sets (Connection, Transfer-Encoding, ...) are dropped, and names lowercased, when encoding.
- **What's left of a request body** when its handler returns isn't read: the stream is reset (RST_STREAM(NO_ERROR)),
  or its remaining DATA ignored, with the connection's window still credited.
- **`close()`** sends GOAWAY(2^31-1), then, after a PING round trip, GOAWAY with the last stream it accepted, lets
  those finish, and closes.

### Timeouts

As Go's, nginx's and Envoy's (see Decided):

- **Per stream:** `body_read_timeout` (a request body that stops arriving) and `write_timeout` (a response that can't
  be sent, including behind a window the peer doesn't open), each from the last progress. On expiry, the stream gets
  408 if nothing was sent yet, otherwise RST_STREAM; the connection goes on. The socket's own deadlines can't do this
  (they're per connection), so a timer task per connection sleeps until the earliest stream deadline and ends the
  late streams.
- **Per connection:** `header_read_timeout` bounds the preface and first SETTINGS, and each request's header block
  once it starts; an idle timeout (no open streams) ends the connection with GOAWAY(NO_ERROR); a SETTINGS that isn't
  acknowledged in time is SETTINGS_TIMEOUT; and if the socket itself can't be written for `write_timeout`, the
  connection closes.

### Limits (the server's defenses)

Every limit is a `server_config` setting with a default from mature servers, and each, when exceeded, ends the stream
or (for floods) the connection with GOAWAY(ENHANCE_YOUR_CALM):

| Attack | Defense (default) |
|---|---|
| Rapid Reset, MadeYouReset | A token bucket for streams reset by the client (nghttp2: burst 1000, 33/s) and one for protocol errors the server resets streams for (nghttp2: burst 10000, 330/s); handlers started but unfinished bounded (Go: 4x MAX_CONCURRENT_STREAMS) |
| CONTINUATION flood, HPACK bomb, zero-length fields | MAX_HEADER_LIST_SIZE enforced as the block arrives, on both the encoded and decoded size (an HPACK bomb expands about 4096:1), and the number of CONTINUATION frames per block (nghttp2: 8) |
| Ping and settings floods | Queued control-frame replies capped (nghttp2: 1000), and settings per SETTINGS frame (Go: 100) |
| Data dribble, empty frames, internal buffering | Empty frames budgeted (h2: 100); per-stream send buffers capped (h2: about 400 KB), and the whole connection's queue |
| Slow peers | The timeouts above; the preface and first SETTINGS within `header_read_timeout` |
| Concurrency | MAX_CONCURRENT_STREAMS 100 (nginx's default; Go's is 250), and `max_connections` as now |

### Client

- **ALPN** offers `h2` and `http/1.1`, and the pool keeps what was agreed per connection. Over cleartext, HTTP/2 is
  only used when asked for (prior knowledge), as no server can be assumed to speak it.
- **The pool** keeps, per scheme, host and port, HTTP/1.1 connections lent one request at a time (as now), and HTTP/2
  connections shared by their requests, each taking a stream: `acquire` hands out a lease that's either. An HTTP/2
  connection takes up to the server's MAX_CONCURRENT_STREAMS (100 assumed until its SETTINGS arrives, as Go and
  Chromium do); then another connection is opened, up to `max_connections_per_host`, and only then does a request
  wait for a stream, within `connect_timeout` (or, with the setting, it waits on the one connection; see Decided). A
  connection's reader coroutine runs for as long as it's open.
- **Responses.** `client_response`'s body reads its stream's DATA (crediting the windows as it goes), and destroying
  it before the end resets the stream (RST_STREAM(CANCEL)) rather than closing the connection.
- **Retries.** A request on a stream after a GOAWAY's last stream, or refused (REFUSED_STREAM), wasn't processed, so
  it's retried on another connection whatever its method. An idle connection is checked with a PING before reuse,
  replacing HTTP/1.1's peek.
- The client sends ENABLE_PUSH 0, and a MAX_HEADER_LIST_SIZE (Go: 10 MB).

### Testing

- HPACK: RFC 7541 Appendix C's examples, hpack-test-case's corpora, Huffman round trips, and the limits (oversized
  integers, padding that isn't EOS's prefix, table-size updates).
- The frame codec and the connection's state machine on bytes, without sockets: every frame type, and the errors the
  RFC requires.
- Loopback: the client and server over TLS and cleartext; many concurrent streams; flow control with a handler that
  doesn't read; per-stream timeouts leaving the other streams alone; GOAWAY and REFUSED_STREAM retries; the client
  opening a second connection at the stream limit; each limit (a test per attack, sending its frames).
- Fuzzing: the frame decoder, HPACK decoding (and that what the encoder makes decodes the same), and the connection
  fed a byte at a time decoding as it does all at once, as `fuzz_http11` does.
- Interop, by hand (and perhaps in CI): h2spec against the server, h2load for load, curl against the server and the
  client against public servers.
- `bench_request` and `bench/web/run.sh` with HTTP/2 (h2load), against the same servers as now; and the primitives'
  benchmark (see Decided).

## Stages

1. **HPACK.** The static table, Huffman coding (the nibble decoder generated at compile time), integers and strings,
   the dynamic table, the decoder (with its limits) and encoder. Tests (Appendix C, hpack-test-case) and a fuzz
   target.
2. **Frames and the connection's state machine**, without I/O: the codec, settings, streams' states, flow control,
   errors, GOAWAY. Tests on bytes, and a fuzz target. The stub (and `util/packed.hpp`) is removed.
3. **The server.** The new primitives in `coro/`, with their tests and the benchmark against `wait_on` (see Decided),
   first; then `response_writer`'s back ends; the connection coroutine, its writer and the
   handlers' tasks; ALPN and prior knowledge; per-stream timeouts; the limits; graceful `close()`. Loopback tests
   (with a minimal test client), h2spec, curl, h2load.
4. **The client.** The pool's shared connections, ALPN, stream limits (and the second connection), retries, PING
   checks. Loopback tests, and public servers (needs the network).
5. **Later:** RFC 9218 priorities (the writer's round-robin becomes urgency-ordered); extended CONNECT (WebSockets);
   an API for trailers; 103 Early Hints; connection coalescing (and the ORIGIN frame, RFC 8336); the encoder's
   dynamic table.

## Out of scope

Server push (browsers dropped it), `Upgrade: h2c` (RFC 9113 dropped it), RFC 7540's priority tree (deprecated; its
frames are parsed and ignored), and HTTP/3 (parked: [http3.md](http3.md)).
