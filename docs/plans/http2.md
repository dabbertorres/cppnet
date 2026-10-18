# Plan: HTTP/2

Status: stages 1 to 4 implemented (2026-10-17): HPACK (`http/hpack.hpp`, with its tests and `fuzz_hpack`); the frames
and session, without I/O (`http/http2.hpp`, `http/http2_session.hpp`, `test/http/http2.cpp`, `fuzz_http2`); the
server (`coro/notifier.hpp` and `bench_notifier`, `http/http2_server.hpp`, `test/http/http2_server.cpp`); and the
client (`http/http2_connection.hpp`, the machinery both sides share, `http/http2_client.hpp`, the pool's HTTP/2
connections, `test/http/http2_client.cpp`). What's left is stage 5's.

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
- **Messages.** A HEADERS frame (and CONTINUATION frames, with nothing else in between, until END_HEADERS), DATA frames,
  and optionally trailers (a second HEADERS frame); END_STREAM on the last. Pseudo-headers come first: `:method`,
  `:scheme`, `:path` and `:authority` for requests (CONNECT: `:method` and `:authority`), `:status` for responses. Names
  are lowercase; values mustn't start or end with a space or tab; Connection, Keep-Alive, Proxy-Connection,
  Transfer-Encoding and Upgrade are malformed, and TE may only be `trailers`. Content-Length must match the DATA's
  length. Cookie may arrive in pieces, to be joined with `"; "`.
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
  streams. (After review:) the connection's window holds every stream's (a server's MAX_CONCURRENT_STREAMS times
  INITIAL_WINDOW_SIZE, at least 1 MiB; the client's 1 GiB, as Go's), and is given back once half of it has been read,
  or as much as the peer has left of it, so bodies read one at a time can't wait on each other's data. A WINDOW_UPDATE
  is written as soon as a read makes one due; a stream's buffer drops what's been read once that's half of it (so it
  holds about twice the window at most); and what a stream received but nobody read, when it's removed or reset for a
  DATA frame, is given back to the connection.
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
  sets (Connection, Transfer-Encoding, ..., and those Connection names) are dropped, names lowercased, and values
  trimmed of the spaces and tabs around them (HTTP/1.1's optional whitespace, which HTTP/2 forbids; a view of the
  value, so nothing's copied), when encoding; names and values are checked as HTTP/1.1's encoder does (so `send()`
  throws for a value with a line break, over either). The client does the same for requests (and the Host it makes
  the `:authority`).
- **What's left of a request body** when its handler returns isn't read: the stream is reset (RST_STREAM(NO_ERROR)),
  or its remaining DATA ignored, with the connection's window still credited. (After review:) a peer's
  RST_STREAM(NO_ERROR) means the same, so if what the peer sent has ended (e.g. a client's response), it stands, and
  only sending stops; otherwise the stream fails with `std::errc::connection_reset`, as a code of 0 can't say it
  failed. A handler that throws once its final head is sent, or writes less than its Content-Length, has its stream
  reset (INTERNAL_ERROR) rather than ended, so the client doesn't take what it got as complete; one that throws before
  (even after a 1xx) gets a 500, without the headers it had set.
- **`close()`** sends GOAWAY(2^31-1), then, after a PING round trip, GOAWAY with the last stream it accepted, lets
  those finish, and closes.

### Timeouts

As Go's, nginx's and Envoy's (see Decided):

- **Per stream:** `body_read_timeout` (a request body that stops arriving) and `write_timeout` (a response that can't
  be sent, including behind a window the peer doesn't open), each from the last progress. On expiry, the stream gets
  408 if nothing was sent yet, otherwise RST_STREAM; the connection goes on. The socket's own deadlines can't do this
  (they're per connection), so a timer task per connection sleeps until the earliest stream deadline and ends the
  late streams. (After review:) each stream has a read deadline (a client's covers its response's head, then its
  body) and a write deadline, both checked by the timer. A wait sets its deadline only if there's none, and only
  progress clears it (data or a head arriving, the stream's window growing, the stream sending some), so waking for
  anything else (e.g. the connection's window growing) doesn't put it off. A write waiting for the connection's writer
  (past 1 MiB queued) has its write deadline too.
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
| CONTINUATION flood, HPACK bomb, zero-length fields | MAX_HEADER_LIST_SIZE enforced as the block arrives, on both the encoded and decoded size (an HPACK bomb expands about 4096:1), and the number of CONTINUATION frames per block (nghttp2: 8; or, if it's more, as many as a block of MAX_HEADER_LIST_SIZE takes in 16 KiB frames, the least MAX_FRAME_SIZE). A block over the encoded size or the CONTINUATIONs ends the connection, as it would have to be decoded anyway, to keep HPACK's table in step; one whose decoded list is over the limit only resets its stream |
| Ping and settings floods | Queued control-frame replies capped (nghttp2: 1000), and settings per SETTINGS frame (Go: 100) |
| Data dribble, empty frames, internal buffering | Empty frames budgeted (h2: 100); per-stream send buffers capped (h2: about 400 KB), and the whole connection's queue |
| Slow peers | The timeouts above; the preface and first SETTINGS within `header_read_timeout` |
| Concurrency | MAX_CONCURRENT_STREAMS 100 (nginx's default; Go's is 250), and `max_connections` as now |

(After review:) handlers are bounded as Go's are (its fix for CVE-2023-44487): at MAX_CONCURRENT_STREAMS running, a new
stream's handler waits for one to finish (and isn't started if its stream is reset meanwhile); past 4 times as many
running and waiting, the connection ends with GOAWAY(ENHANCE_YOUR_CALM), whose last stream is the last whose handler was
started (so the client may retry the rest), and the stream that tripped it is dropped. A connection that ends with
streams in flight (that GOAWAY, any other connection error, a failed write, or the client closing) is shut down
(close_notify over TLS, then FIN) once the writer has flushed what's left, before its handlers finish: RFC 9113 (section
5.4.1) says it MUST be closed after a connection error, and a client waits for the streams GOAWAY says may have been
processed until it is. Its fd, and its `max_connections` slot, are still held until the handlers return, deliberately:
as handlers needn't heed their tokens (`server_request::stop`, which the connection's end stops: see
`handler-cancellation.md`), that bounds those left running to `max_connections` times `max_concurrent_streams`
(freeing the slot would let a client reconnect and pile up more), and HTTP/1.1 holds its slot the same way. Graceful
`close()` is unchanged. (Later:) however a connection ends (the client's side too), its reader then goes on reading, and
dropping, what the peer still sends, until the peer closes its side too, for up to 2 s (or 256 KB), as HTTP/1.1 lingers
after an error response: closing with input unread, or (on macOS) input arriving after the reading side was shut down
(as `end()` did), resets the connection, which discards what's still on its way to the peer, e.g. the end of a response
while the client sends WINDOW_UPDATEs for what it's read. A malformed PRIORITY (the wrong size, or a stream depending on itself) resets an open stream, as
a stream error the application is told of; on a stream that isn't open, it only counts as a glitch, as RST_STREAM on an
idle stream would be a connection error for the peer.

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

- HPACK: RFC 7541 Appendix C's examples, some of hpack-test-case's stories (a series of requests and one of responses,
  as encoded without Huffman coding, with it, and with the table's size changing between blocks; generated into
  `test/http/hpack_stories.hpp` by `tools/hpack/gen_stories.py`), Huffman round trips, and the limits (oversized
  integers, padding that isn't EOS's prefix, table-size updates).
- The frame codec and the connection's state machine on bytes, without sockets: every frame type, and the errors the
  RFC requires.
- Loopback: the client and server over TLS and cleartext; many concurrent streams; flow control with a handler that
  doesn't read; per-stream timeouts leaving the other streams alone; GOAWAY and REFUSED_STREAM retries; the client
  opening a second connection at the stream limit; each limit (a test per attack, sending its frames).
- Fuzzing: the frame decoder, HPACK decoding (a connection's blocks, with table size changes between them; and that
  what the encoder makes decodes the same), and the connection fed a byte at a time decoding as it does all at once,
  as `fuzz_http11` does: the input is a script, of frames to receive, and what the application does between them
  (a client's requests, a server's responses, data, resets, consuming, PING, GOAWAY) and time passing.
- Interop, by hand (and perhaps in CI): h2spec against the server, h2load for load, curl against the server and the
  client against public servers.
- `bench_request` and `bench/web/run.sh` with HTTP/2 (h2load), against the same servers as now; and the primitives'
  benchmark (see Decided).

## Learned in stage 1

- The static table and the Huffman code are generated from RFC 7541's text (Appendices A and B), checking as they're
  parsed that each code's bits match its value and length, and that the code is complete; the tests' examples
  (Appendix C, with the dynamic table after each block) are generated the same way. The Huffman decoder's table (256
  states, one per internal node of the code's tree, by 16 nibbles) is built at compile time, and a `static_assert`
  checks the tree has 256 internal nodes.
- A never-indexed field must stay a never-indexed literal when re-encoded, even if the static table has it (e.g.
  `:authority` with an empty value): as an index, whoever decodes it next wouldn't know not to index it (RFC 7541,
  section 6.2.3). Fuzzing found this, by checking that what's decoded, encoded again, decodes the same.
- The decoder takes a whole header block (HEADERS and its CONTINUATIONs, joined), rather than decoding as frames
  arrive: so the frames' layer (stage 2) bounds the encoded block (its size, and the number of CONTINUATIONs), and the
  decoder bounds the decoded header list (an HPACK bomb is decoded to the end, to keep the dynamic table in step, but
  without passing on its fields).
- A decoder starts with its dynamic table at the size it's given, as both sides start with the same
  SETTINGS_HEADER_TABLE_SIZE (4096, unless the RFC's examples say otherwise); `set_max_table_size()` lowers the limit
  once the peer has acknowledged this side's, and then the next block must start with a size update.

## Learned in stage 2

- `http2::session` is the protocol without I/O: `receive()` takes what's been read and uses only whole frames
  (returning how much, so the rest is passed again with what's read next), reporting to a `session_events`
  interface; the application's side is `open_stream()`, `submit_headers()`, `submit_data()` (as far as the windows
  allow), `reset()`, `consume()` (giving windows back), `goaway()` and `ping()`; and what's to be written is
  `take_output()`'s. It isn't thread-safe: the server and client serialize calls into it (stage 3). Fuzzing checks it
  receives frames the same at once or a byte at a time.
- The defenses that count frames live in the session, where they're tested on bytes, rather than in the server: a
  token bucket for the peer's resets (Rapid Reset) and one for "glitches" (stream errors it causes, empty frames,
  refused streams: MadeYouReset); CONTINUATIONs per block and the block's encoded size; control-frame replies waiting
  to be written; settings per SETTINGS frame. Each fails the connection with ENHANCE_YOUR_CALM.
- It validates header blocks by RFC 9113's rules (lowercase, valid names and values (no controls but tab, as HTTP/1.1's:
  RFC 9113 only forbids NUL, CR and LF, but a proxy may pass them on over HTTP/1.1; nor a space or tab at either end),
  pseudo-headers first and only the right ones, no connection-specific fields, TE only `trailers`, at most one Host,
  agreeing with `:authority` but for case, a request's or response's required pseudo-headers, Content-Lengths that
  agree), and checks DATA, and trailers that end the stream, against Content-Length (except for responses to HEAD, 204
  and 304). A malformed block (or HEADERS whose priority depends on its own stream), or one whose decoded list is over
  MAX_HEADER_LIST_SIZE, resets its stream with PROTOCOL_ERROR (the server could answer 431 instead, later), once it's
  decoded. A block whose encoded size or CONTINUATIONs are over their limits ends the connection (ENHANCE_YOUR_CALM), as
  it would have to be decoded anyway, to keep HPACK's table in step.
- Closed streams are told from idle ones by the highest ids each side has opened, without keeping them; the last 128
  streams this side reset are remembered, so that what the peer sent before it saw the reset is ignored rather than an
  error (RFC 9113, section 5.1). HEADERS on a closed stream that isn't remembered is a stream error (STREAM_CLOSED),
  not the connection's, as its reset may just have been forgotten (as Go's). After a server's GOAWAY, the frames of
  the client's streams past its last are ignored, without counting them as glitches (the client may not have seen
  it): DATA gives its length back to the connection, and HEADERS is decoded, to keep HPACK in step. A connection
  error's GOAWAY never raises the last stream of one sent before.
- Until the server's SETTINGS arrive, a client opens at most 100 streams (as Go and Chromium assume). A session's own
  INITIAL_WINDOW_SIZE is at least 65535, as the peer may send that much before it has seen this side's SETTINGS.
- `util/packed.hpp` went with the stub: nothing else packed structs.

## Learned in stage 3

- No queue for the writer, after all: handlers submit their heads and data to the session directly, under the
  connection's lock (the session keeps its output in the order it was submitted, so HPACK's encoding stays in step),
  and the one writer per connection drains the session's output to the socket. Only one primitive was needed:
  `coro::notifier`, which body writes wait on for windows (and for the writer, if more than 1 MiB is waiting), and
  the writer for output. `bench_notifier` compares it with `wait_on()`: about the same with one worker, and up to
  about 3x faster with many coroutines on several workers.
- The session's events run under the connection's lock, so nothing they do may take it again: handlers aren't
  started from them (the new streams are started once it's released, each hopping onto the pool, so the reader goes
  on reading), and notifiers resume their waiters on the pool, never inline. (After review:) a pool refusing them (as
  it's shutting down) terminates the program, rather than resuming them inline: it can't happen while the scheduler
  holds the pool, as it waits for its tasks (which own the notifiers) before letting it go.
- `response_writer`'s back end is a `std::variant`; the HTTP/2 alternative holds the stream and a table of the
  server's functions (as the HTTP/1.1 one holds an encoder function), so that what uses `response.cpp` alone (e.g.
  `fuzz_http11`) doesn't need HTTP/2's server and the scheduler linked in.
- Per-stream timeouts are a timer task per connection (the one place that waits with the scheduler's `wait_on()`, as
  it needs a deadline), which resets a stream whose body read or write has made no progress for its timeout
  (RST_STREAM(CANCEL)), rather than sending 408 (nginx, too, ends such a stream without a response). The connection's
  own timeouts reuse `header_read_timeout`: for the preface, and for a connection with no streams (as HTTP/1.1's wait
  for a kept-alive connection's next request does). The socket's read timeout is cleared for HTTP/2: its reader waits
  as long as the connection's open.
- The server's MAX_HEADER_LIST_SIZE is `max_header_bytes` plus 32 bytes for each of up to 100 fields (HTTP/1.1's
  limit on fields), as HPACK counts 32 more per field than the text (saturating, for a `max_header_bytes` near
  SIZE_MAX).
- `close()` is graceful: GOAWAY with the highest stream id and a PING, then the final GOAWAY once the PING's
  acknowledged (or after a second), and the connection ends once its streams are done.
- Over cleartext, `serve_requests` peeks for the preface before reading a request (without a coroutine when what's
  buffered, or a read that doesn't wait, tells; within the first request's `header_read_timeout`, not one of its own);
  HTTP/1.1's `close()` callback (which ends reads) is removed before handing the connection to HTTP/2, which registers
  its own (graceful) one. Over TLS, `tls::stream::accept()` now takes the server's ALPN protocols, so the server offers
  `h2` whatever its context's options say.
- Request trailers end the body, but aren't passed on (there's no API for them yet).
- h2spec and h2load weren't run (they aren't installed here); curl (built with nghttp2) was, over cleartext and TLS.

## Learned in stage 4

- Both sides' connections share a base, `http2_connection_base` (`http/http2_connection.hpp`): the reader that feeds
  the session, the writer that drains it, the timer task for per-stream timeouts, the registry of streams, and the
  streams' body reader and writer (`stream_reader`, `stream_writer`). Each side adds its events (a server starts
  handlers; a client wakes the request waiting for its response's head) and its connection-level checks (the server's
  preface and idle timeouts, and graceful close; the client's idle timeout).
- A client's HTTP/2 connection is owned by its own task (holding a `shared_ptr`), and by the pool's list for its host;
  responses' bodies hold it too, so it lives until the last is destroyed. The pool hands out a lease that's either an
  HTTP/1.x connection or an HTTP/2 one, and a connection with a stream free (by the server's MAX_CONCURRENT_STREAMS,
  which `session::can_open_stream()` checks) is preferred to making another. With
  `client_options::strict_max_concurrent_streams`, a request waits for a stream instead; a stream being done (or the
  connection closing) wakes the pool's waiters, as an HTTP/1.x connection being released does. (After review:) so
  does the session being done with a stream (its response ended, or reset), though its response hasn't been read; and
  a request that fails before its response's head (refused, reset, timed out) is done with its stream at once. The pool reserves the
  stream when it hands out the lease (`http2_client_connection::reserve()`, which `session::can_open_stream(reserved)`
  counts), as the stream is only opened later: without that, two requests could both take the last free stream, and
  one be refused by its own side.
- No PING check before reusing an HTTP/2 connection, after all: its reader is always reading, so a connection the
  server closed is noticed (and dropped from the pool) as it happens, unlike an idle HTTP/1.1 connection, which
  nothing reads.
- A request that's refused (REFUSED_STREAM, or after the server's GOAWAY) is sent again once, on another connection,
  if it has no body (whose reader can't be rewound), whatever its method. One the server sent headers for was
  processed, so its refusal is reported as CANCEL, which isn't retried.
- (After review:) the pool's other edges. A retry goes on another connection than the one that refused it (`acquire`
  skips it). A request refused before its stream was opened (the connection was ending, or had the server's GOAWAY)
  was sent nothing of, so it's retried even with a body; and one safe to repeat, without a body, whose connection
  dropped without GOAWAY before its response's head is retried as HTTP/1.1's are on a reused connection that turns out
  closed. A reserved stream counts against the connection being idle (for the idle timeout, eviction and stats), and
  `idle_timeout = 0` closes a connection with its last stream, as HTTP/1.1's are. At `max_connections`, an idle HTTP/2
  connection to another host may be evicted (counted out at once: it's dropped from its host's list, so its `closed`
  callback doesn't count it out again). The pool remembers the hosts that agreed on `h2` (or have prior knowledge),
  and while a connection to one is being made, other requests wait for it rather than each connecting. Strict
  requests wait only for connections that may still open streams, and the server's GOAWAY wakes them. Destroying the
  pool drains its connections (GOAWAY, then the end with the last stream) rather than cutting off bodies being read.
  The pool sets an HTTP/2 connection's socket's write timeout to `io_timeout`, so a writer stuck behind a server that
  doesn't read fails the connection. `:authority` is the caller's `Host` (which isn't sent as a field), or the URI's
  without the scheme's default port; a body with a Content-Length is sent to that length (failing if it's shorter),
  and a body reader that returns nothing fails with `no_progress`, as over HTTP/1.1.
- `client_options::http2` (on by default) offers `h2` over TLS; `http2_prior_knowledge` speaks it over cleartext.
  Response trailers end the body, but aren't passed on (as with requests' on the server).
- `server_config::http2_max_concurrent_streams` (100, as nginx's) was added, for the client's stream-limit tests.
- (After review:) the public headers don't include HTTP/2's internals: what the configurations hold (`settings`,
  `token_bucket`, `session_config`, and the server's and client's options) is in `http/http2_config.hpp`, which
  `http/server.hpp` and `http/connection_pool.hpp` (so `http/client.hpp`) include, forward-declaring the connections;
  `http/http2.hpp` and `http/http2_session.hpp` include it too, so what used them from there still builds. The pool
  counts the requests waiting for a connection (or stream) to be freed, so a stream ending wakes the scheduler's
  waiters (behind its lock) only if there are any. The client's extra frame per request (`send_http11`'s, an
  allocation, against a network round trip) stays.
- No public servers in the tests: the HTTPS test is against the loopback server, which agrees on `h2`.

## Stages

1. **HPACK.** The static table, Huffman coding (the nibble decoder generated at compile time), integers and strings,
   the dynamic table, the decoder (with its limits) and encoder. Tests (Appendix C, hpack-test-case) and a fuzz
   target.
2. **Frames and the connection's state machine**, without I/O: the codec, settings, streams' states, flow control,
   errors, GOAWAY. Tests on bytes, and a fuzz target. The stub (and `util/packed.hpp`) is removed.
3. **The server.** The new primitive in `coro/` (`coro::notifier`), with its tests and the benchmark against `wait_on`
   (see Decided and Learned in stage 3), first; then `response_writer`'s back ends; the connection coroutine, its writer and the
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
