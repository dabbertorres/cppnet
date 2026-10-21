# Plan: HTTP/2

Status: stages 1 to 4 implemented (2026-10-17): HPACK (`http/hpack.hpp`, with its tests and `fuzz_hpack`); the frames
and session, without I/O (`http/http2.hpp`, `http/http2_session.hpp`, `test/http/http2.cpp`, `fuzz_http2`); the
server (`coro/notifier.hpp` and `bench_notifier`, `http/http2_server.hpp`, `test/http/http2_server.cpp`); and the
client (`http/http2_connection.hpp`, the machinery both sides share, `http/http2_client.hpp`, the pool's HTTP/2
connections, `test/http/http2_client.cpp`). Stage 5 decided (2026-10-20): interim responses (Early Hints) checked
end to end, trailers, the HPACK encoder's dynamic table, and RFC 9218's priorities, in ten pull requests (see "Stage
5"); its questions (at the end) all as recommended. Its first part, interim responses on the server (and 100 Continue),
done (2026-10-20; see Learned in stage 5); its second, interim responses in the client (`on_informational`), done
(2026-10-20); its third, trailers in the codecs (`chunked_reader`, `chunked_writer::finish(trailers)`,
`trailer_allowed()`), done (2026-10-20).

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
  the dynamic table is where compression oracles live. It may come later, opt-in per field. (Stage 5 proposes it.)
- **No server push** (browsers dropped it), and RFC 7540's priority tree is parsed and ignored (RFC 9113 deprecated
  it); RFC 9218's priorities may come later (stage 5 proposes them).

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
  `PRIORITY_UPDATE`): optional, as priority is only a suggestion. Stage 5. Its fields are Structured Fields:
  [RFC 9651](https://www.rfc-editor.org/rfc/rfc9651.html), which obsoletes RFC 8941.
- [RFC 8297](https://www.rfc-editor.org/rfc/rfc8297.html), 103 Early Hints, and
  [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html), HTTP/1.1 (its chunked trailers, section 7.1.2). Stage 5.
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

## Learned in stage 5

### Part 1: interim responses, the server (done 2026-10-20)

- The body's "first read" hook is `io::start_callback` (`io/reader.hpp`), `io::end_callback`'s sibling: awaited by the
  first read into a non-empty span of `io::limit_reader`, `http11::chunked_reader` and HTTP/2's `stream_reader`, before
  it reads, its error failing that read. It's a task, not a plain function as `end_callback` is, as sending the 100 may
  have to wait (over HTTP/1.1, the connection's buffer may hold pipelined responses, which go first). The server sets it
  with `http11::call_at_body_start()` (beside `call_at_body_end()`), or on the stream's reader. Unset, it's a branch per
  read; the server looks for an `Expect` field once per request (`detail::expectation_of()`, in `http/request.hpp`).
- `response_writer` keeps the expectation, so both versions share the rule: the server calls `expect_continue()`
  (for a request with `Expect: 100-continue` whose body is to come: chunked, or a Content-Length over 0, over HTTP/1.1;
  a head without END_STREAM over HTTP/2), which returns the callback, and `continue_unmet()` says how it ended. The
  100 is just the status (no `Date`, nor the headers the handler has set so far), as Go's, and isn't the handler's:
  `status_code()` doesn't change. A body partly sent already still gets one at its first read (as Go's: RFC 9110's
  MAY omit isn't taken).
- A handler's own `send(status::CONTINUE)` meets the expectation. A final response first leaves it unmet, and the
  body's first read then sends nothing; reading the body after that clears it: the client sends the body once it stops
  waiting (curl after a second). Another interim response (a 103) does neither: the 100 is still owed, and the body's
  first read sends it after the 103. Decided question 3 said "if nothing has been sent yet", which part 1 first
  followed (a 103 stopped the 100 too); but Go's server only stops its own 100 for the handler's 100 or the body's first
  write (its final response), so a 103 then a read gets a 100 there. The user chose Go's rule (2026-10-20).
- Over HTTP/1.1, a final response sent while the 100 is owed or unmet says `Connection: close` (RFC 9110, section
  10.1.1: a server answering before reading the content SHOULD say whether it closes; Go's `closeAfterReply`); and if
  the handler hasn't read the body by the time it's done, the server doesn't discard it (the client may never send it,
  and discarding would wait up to `body_read_timeout`): it flushes, lingers as after an error response (FIN, then reads
  and drops for up to 2 s, so a body sent after all doesn't reset the connection before the response is read), and
  closes. Over HTTP/2, the stream's reset (NO_ERROR) as for any body left unread.
- `Expect` is a list: each member must be `100-continue` (in any case; it has no parameters) or the expectation is
  unknown, answered 417 before the handler runs (so neither it nor middleware sees it, as with a 400): over HTTP/1.1
  with `Connection: close`, then lingering (Go's `sendExpectationFailed`); over HTTP/2 as the server refuses a stream
  (with RST_STREAM(NO_ERROR) if the request hadn't ended). An empty `Expect` is none. HTTP/1.0's `Expect` is ignored
  altogether, an unknown expectation too (HTTP/1.0 has none; Go answers 417 to that one).
- An interim response over HTTP/1.1 is a coroutine (`send_interim()`): the head's encoded into the connection's
  buffer as before, then flushed (through a `dynamic_cast` to `io::buffered_writer`, as the encoder already does);
  a flush that fails throws `std::system_error`, as a head HTTP/2 can't send does. To an HTTP/1.0 client, `send()` of
  a 1xx returns a `discard_writer` and sends nothing (`status_code()` unchanged, the headers kept for the final
  response). `send(status::SWITCHING_PROTOCOLS)` throws before anything, over either version, to an HTTP/1.0 client
  too. Interim responses still carry the response's headers so far, `Date` among them.
- (A fix found on the way:) an HTTP/1.1 handler that threw after sending only interim responses got no 500 (the server
  checked `status_code() == status::NONE`, which a 1xx changes), so the client saw the 1xx and then the connection
  close; it now gets the 500 (checking `sent()`), as over HTTP/2.
- The tests' `test::h2_client` records each stream's informational heads (`interims`).
- `bench_request`'s own serving loop (for its pipelined runs) looks for the expectation as the server does. Measured
  (Release, five alternating runs of the binaries before and after, best of each, on a noisy laptop): allocations
  unchanged everywhere (1.4 per request pipelined, 4.2 one per read, 3.0 over a socket, 2.0 to a handler that waits,
  about 52 over HTTP/2); times within the runs' spread (pipelined 1314 ns before, 1353 after, each varying by 15%
  from run to run; one per read 1446 and 1412; over a socket 33.0 and 32.8 us). The `Expect` lookup is at most a few
  tens of nanoseconds of the pipelined 1.3 us.

### Part 2: interim responses, the client (done 2026-10-20)

- `client_request::on_informational` is as proposed, a `std::function<void(status, const headers&)>` (`status` is
  declared opaquely in `request.hpp`, as `response.hpp`, which defines it, includes `request.hpp`). Over HTTP/1.1,
  `http11::response_decode()` takes it as an `http11::informational_ref`, a `util::function_ref` (new,
  `util/function_ref.hpp`: two pointers, as C++26's `std::function_ref`, which C++23 lacks; unlike it, nullable, and
  not made from a temporary, which a coroutine taking it would outlive). The decoder calls it outside its `try`, so
  whatever it throws (even `std::bad_alloc`, which the decoder's own allocations report as `not_enough_memory`) comes
  out of the task as it is. `response_decoder` (the function pointer type) takes it too.
- HTTP/1.1's `send_http11` always passes a lambda of its own, which notes that an interim head came and calls the
  request's callback, if it has one: a request that got an interim response isn't sent again if its reused connection
  then ends, as the server had it (HTTP/2 already behaved so: any HEADERS marks its stream `processed`, and Go's
  HTTP/1.1 retries only a failure of the response's first byte). Without a callback the decoder still parses an interim
  head's fields, as it did, so the lambda costs nothing more.
- HTTP/2: the session already reported informational blocks (`block_kind::informational`, `:status` among the fields);
  the client connection dropped them. Now `on_headers()` counts every block's fields (`hpack::field_size`, `:status`
  included, as Go's `Fields` and the RFC count them) into the stream's `interim_size`, whether or not the request has a
  callback, and past the session's `max_header_list_size` (the pool's 1 MiB + 3200) fails the stream with
  `std::errc::value_too_large` and resets it with PROTOCOL_ERROR (Go's code for the stream error, and the session's for
  one block over the limit). Go bounds them only when no trace function takes them; here they're queued until
  `send()`'s task passes them on, so they're bounded either way. A request with the callback (`wants_interim`, set in
  `open()`) has its heads queued on the stream (`interims`, each a status and `headers`: only those allocate) and the
  stream notified; `wait_for_head()` swaps the queue out under `mu` and calls the callback without it, before it looks
  at the final head or a failure, so the heads that came before a failure (the limit's too) are still passed on, in
  order. Informational blocks still never clear the stream's read deadline, nor does waking for them re-arm it, so
  `response_header_timeout` is the final head's (HTTP/1.1's is a socket deadline, set before decoding).
- An exception from the callback unwinds `send()`: over HTTP/1.1, `pooled_body`'s destructor closes the connection;
  over HTTP/2, `send()`'s `defer` calls `done()`, which resets the stream (CANCEL), the connection kept. The tests check
  the server sees it (its handler's token is stopped), and that the next request is answered.
- Redirects: the callback stays on the request `redirect()` rewrites, so each hop's interim responses are passed on, as
  Go calls `Got1xxResponse` for each.
- The client never sends `Expect: 100-continue` by itself (waiting for 100 before the body stays "still later"); a
  request that sets it gets the server's 100 passed on, but its body goes at once.
- Measured (Release, five alternating runs of the binaries before and after): over HTTP/2 (cppnet's client and server),
  52.0 allocations a request before and after (51.9 to 52.2 from run to run, either way), 80.6 to 83.8 us before, 76.9 to
  79.8 after (noise: nothing on that path changed but a branch, and a vector's swap per wait); and over HTTP/1.1 with
  cppnet's client (`bench_request`'s HTTP/2 run with prior knowledge off, a throwaway change), 28.0 allocations both,
  42.3 to 45.1 us before, 42.8 to 44.0 after. The server's runs are unchanged (1.4, 4.2, 3.0 and 2.0 allocations).

### Part 3: trailers in the codecs (done 2026-10-20)

- `http::trailer_allowed(name)` is in `http/headers.hpp` (public, as proposed): Go's list, an `If-` prefix (ignoring
  case) and the 21 names, compared ignoring case one after another; only called for a message that has trailers.
- `chunked_reader` takes the `headers*` by a setter, `keep_trailers()`, as it takes its callbacks (`call_at_end()`,
  `call_at_start()`), rather than as a constructor argument: the client's response body is made inside
  `response_decode()` (`make_response_body()`), before the `pooled_body` whose trailers it's to fill exists, so part 5
  needs to set it afterwards anyway; part 4 can set it beside `call_at_body_end()` (or pass it through
  `reuse_or_make()`, which a setter doesn't stop). Null by default, so the server and client don't keep any yet.
- What the reader keeps: the section replaces what the headers held (cleared as it starts), a repeated name is one field
  with each value in order (as `headers::add()` keeps them), and a section that fails (malformed, too long, or cut
  short) leaves them empty, so no one sees part of one. A `std::bad_alloc` adding one fails the read with
  `not_enough_memory`.
- The section's lines are now limited to `http11::max_fields` whether they're kept or not, and the fields dropped count
  too, so a body decodes the same either way (the fuzzer's property); before, only `max_trailers` (8 KiB) limited it,
  which let some 2,000 tiny lines through. As in a head, the line past the limit fails with `value_too_large` before
  it's parsed, malformed or not.
- `chunked_writer::finish(const headers* trailers = nullptr)` checks every field with `valid_fields()` before it writes
  anything, those it would drop too (a line break in one is the caller's bug either way; Decided question 12). It
  formats the last chunk into an `io::buffered_writer`'s buffer (`prepare()`, found with a `dynamic_cast`, as
  `response_encode()` finds it) and returns a ready task, otherwise writes it as one string. Before, `finish()` was
  always a coroutine awaiting another (`write_full()`), so a chunked response's end now usually needs no frame; the
  server's chunked responses gain that, though `bench_request` (whose responses have a Content-Length) doesn't show
  it. A null writer (`response_writer`'s placeholder `chunked_writer{nullptr}`) fails with `invalid_argument`.
- `fuzz_http11`: until part 4 gives `server_request` its trailers, the fuzzer finds a chunked body with a
  `dynamic_cast` and calls `keep_trailers()` itself. The transcript has each request's trailers (`trailer name:
  value`), each checked to be a valid field and allowed; they're written back with `finish()` and read back the same
  (`operator==`); and a body that fails must have kept none. Ran for 150 s from a few seeds with trailers (127,000
  runs), as well as its ctest run.
- Measured (`bench_request`, Release, five alternating runs of the binaries before and after, best of each):
  allocations unchanged everywhere (1.4 per request pipelined, 4.2 one per read, 3.0 over a socket, 2.0 to a handler
  that waits, 52.0 over HTTP/2, and the middleware runs' 2.4, 1.4, 2.4, 3.4, 2.4 and 4.0). Times: over a socket 27.7
  us both, to a handler that waits 36.4 and 36.3, over HTTP/2 80.8 and 79.2; pipelined 1110 ns before and 1118 after,
  one per read 1302 and 1319, and the pipelined middleware runs 1 to 3% slower at best, within or at the edge of each
  run's spread (1 to 4%). None of those requests or responses is chunked, so nothing they run changed; it's taken for
  noise, or code layout.

## Stage 5

Decided (2026-10-20). Four things, chosen for stage 5: interim (1xx) responses checked end to end, Early Hints among
them; an API for trailers; the HPACK encoder's dynamic table; and RFC 9218's priorities. Go's behaviour is from its
sources as of October 2026 (Go 1.27 moved HTTP/2 into the standard library, `src/net/http/internal/http2`; HPACK is
still x/net's, vendored as `src/vendor/golang.org/x/net/http2/hpack`); nghttp2's, Rust's h2 and hyper's, h2o's and
curl's from their current sources. What's from memory says so. The ten pull requests they're split into, and why each
split is where it is, are in Stages.

### Interim responses and Early Hints

**Where things stand.**

- `response_writer::send()` takes any number of 1xx before the final response: they don't count as it (`sent()`),
  drop Content-Length (`response.cpp`), and keep the headers, so what a 103 hinted is sent again with the final
  response (as RFC 8297 expects, and Go does: "Per RFC 8297 we must not clear the current header map"). They carry the
  `Date` the server set before the handler, which is harmless.
- Over HTTP/2 they work: `http2_send_head()` (`http2_server.cpp`) submits a HEADERS frame without END_STREAM and wakes
  the writer, so a 103 goes out at once.
- Over HTTP/1.1 they don't: `http11::response_encode` formats the head into the connection's `buffered_writer`, which
  is flushed only when the server next reads (`io::flush_before_read`) or the buffer fills. So a 103 reaches the client
  in the same write as the final response, which defeats it. (A handler that reads its body after a 100 flushes it, as
  the body's reads go through `flush_before_read`.)
- A 1xx goes to an HTTP/1.0 client too, which RFC 9110 forbids (section 15.2: "a server MUST NOT send a 1xx response to
  an HTTP/1.0 client"); and `send(status::SWITCHING_PROTOCOLS)` is taken as an interim response, which tells an HTTP/1.1
  client the connection has switched protocols (nothing can switch: `Upgrade` is ignored), and is malformed over HTTP/2
  (RFC 9113, section 8.6: "HTTP/2 does not support the 101 (Switching Protocols) informational status code").
- `Expect: 100-continue` isn't handled: the server never sends 100 by itself. RFC 9110 (section 10.1.1) says an origin
  server receiving it with content to follow "MUST send either" an immediate final response or "an immediate 100
  (Continue) response", and clients wait for it before sending the body (curl, from memory: for a body over 1 MiB, for a
  second).
- The client skips 1xx: HTTP/1.1's `response_decode` loops past them (all of a response's heads, interim and final,
  within one `max_header_bytes`), returning a 101 as final (its body is the connection); HTTP/2's
  `http2_client_connection::on_headers` drops `block_kind::informational` without resetting the stream's read deadline,
  so a server sending 1xx for ever runs into `response_header_timeout`, but their count and size aren't otherwise
  bounded; the session rejects a 101 (a stream error) and a 1xx with END_STREAM. Nothing passes them to the caller.

**What the RFCs require.** RFC 9110, section 15.2: a client "MUST be able to parse one or more 1xx responses received
prior to a final response, even if the client does not expect one", and "MAY ignore" them; a proxy must forward them.
RFC 8297: a 103's fields are hints, which the client may act on (e.g. `Link: </style.css>; rel=preload`), but which
"MUST NOT affect how the final response is processed"; a server may send several; its security considerations warn that
an HTTP/1.1 client that takes a 1xx for the final response would misread the rest of the connection. RFC 9113, section
8.1: any number of interim HEADERS before the final one, none with END_STREAM.

**How others do it.**

- Go's server: `WriteHeader(code)` for 100 to 199 but 101 writes the status line and the headers set so far (but
  Content-Length and Transfer-Encoding), keeps them, and flushes at once (`server.go`, `response.WriteHeader`); over
  HTTP/2, a HEADERS frame at once. It doesn't check for HTTP/1.0 clients. It sends 100 Continue by itself when the
  handler first reads the body of a request that expects it (and a `WriteHeader(100)`, or a final response, turns that
  off).
- Go's client: `httptrace.ClientTrace.Got1xxResponse(code, header) error` is called for each 1xx before the final
  response, 100 included (an error from it ends the request), and `Got100Continue`; otherwise they're skipped. HTTP/1.1
  bounds all of a response's heads together by `MaxResponseHeaderBytes` (10 MiB), and HTTP/2 the 1xx heads' total size
  by the larger of that and MAX_HEADER_LIST_SIZE ("header list too large"), unless a trace function is set, which then
  answers for them. There's no limit on their count any more.
- hyper's client has `hyper::ext::on_informational(&mut req, callback)`; curl passes 1xx heads to the header callback
  (`CURLH_1XX`), and otherwise ignores them. h2o's server sends 1xx only to HTTP/2 and HTTP/3 clients by default
  (`send-informational: except-h1`), for RFC 8297's concern about old HTTP/1.1 clients.

**Proposed.**

- Server, HTTP/1.1: `send()` of a 1xx flushes the connection's writer once the head's in it (so it's a coroutine then;
  interim responses are rare), and does nothing for an HTTP/1.0 request (returning the discarding writer: Decided
  question 1). The flush is the handler's own write (no other task writes while the handler runs; the watch's read is
  held, and doesn't flush), so it races with nothing.
- Server, both versions: `send(status::SWITCHING_PROTOCOLS)` throws `std::system_error` with
  `std::errc::not_supported` (as for a head that can't be sent), until something can switch protocols (Decided question
  2).
- Server, both versions: 100 Continue sent by itself (Decided question 3): for a request with `Expect: 100-continue`
  (not HTTP/1.0, whose expectation is ignored) whose body is to come, when the handler first reads the body, unless the
  handler has sent its own 100 or the final response by then (another 1xx, e.g. a 103, doesn't stop it), as Go does
  (this said "if nothing (no 1xx, no final response) has been sent yet", which isn't Go's rule: corrected 2026-10-20,
  the user choosing Go's; see Learned in stage 5); over HTTP/1.1 written and flushed before the read, over HTTP/2 a
  HEADERS frame. A handler that answers without reading (e.g. 413 for a body too large) sends no 100, and its
  connection closes after the response if the body's left unread (as now). An expectation other than `100-continue`
  is answered 417 (RFC 9110 says MAY; Go does) before the handler runs. The body readers learn of it through the
  callback they already take at their end (`io::end_callback`), generalized to a "first read" hook on the request's body
  (`http11::call_at_body_end()`'s sibling), so a request without the expectation pays nothing.
- Client: a callback on the request, called for each 1xx before the final response (Decided question 4):

  ```cpp
  struct client_request
  {
      // ... as now, and:
      // on_informational is called for each interim (1xx) response before the final one (100 Continue and 103 Early
      // Hints among them; not 101, which is final over HTTP/1.1, and malformed over HTTP/2), with its status and
      // fields, which are only valid during the call. It's called on the task sending the request, so must be quick;
      // an exception it throws comes out of send(), the request abandoned (its connection closed, or stream reset).
      std::function<void(status, const headers&)> on_informational{};
  };
  ```

  HTTP/1.1's `response_decode` takes it (as a function reference, so the codec doesn't grow a `std::function`
  parameter); HTTP/2's stream keeps the fields of each informational block and calls it on `send()`'s task (not under
  the connection's lock, where `on_headers()` runs), so the stream queues the heads it hasn't been told of yet, woken as
  for its final head. A response's interim heads are bounded together: over HTTP/1.1 by `max_header_bytes` with the
  final head (as now), over HTTP/2 by the session's `max_header_list_size` (1 MiB and 32 a field, the client's), past
  which the stream's reset and the request fails with `std::errc::value_too_large` (as Go's). A 1xx doesn't count as
  progress towards `response_header_timeout` (as now), whose subject is the final head.
- The guide's server and client sections say how 1xx work (a handler's Early Hints: `rw.headers().add("Link", ...)`,
  `co_await rw.send(status::EARLY_HINTS)`, then the final response; the client's callback), and `status` gets
  `EARLY_HINTS = 103` (it has `PROCESSING`, 102, but not 103).

**Costs.** Nothing for responses without a 1xx (the flush is only after one), nor for requests without
`Expect: 100-continue` (a branch when the request's decoded) or without a callback. A 103 over HTTP/1.1 costs a write
of its own, which is its point.

**Tests** (no network). HTTP/1.1, over a socket with `blocking_client`: a handler sends 103 with a `Link`, then waits
for a signal the test gives once it has read the 103 (so it can't arrive with the final response), then a 200 that still
has the `Link`; several 103s; a 100 Continue by itself, before the body's read, for a request with the expectation
(after a 103 too; and none without it, none to HTTP/1.0, none after a final response that didn't read the body); 417 for
an unknown expectation; no 1xx to an HTTP/1.0 request; `send(101)` throws. HTTP/2 the same with `test::h2_client` (the
103's HEADERS before the final one's, and no END_STREAM on it). The client, over both versions against the server:
`on_informational` sees each 103's status and fields in order, before `send()` returns; without it they're skipped; an
exception from it fails the request; an HTTP/2 server sending more interim heads than the limit (by `test::h2_client` in
its server role) fails the request with `value_too_large`.

### Trailers

**Where things stand.**

- HTTP/1.1: `http11::chunked_reader` reads the trailer section after the last chunk, checks each line
  (`parse_field_line()`) within `max_trailers` (8 KiB), and drops them; `chunked_writer::finish()` writes `0\r\n\r\n`,
  so nothing can be sent. The client's request bodies of unknown length are sent chunked (`copy_chunked()` in
  `http11.cpp`), the same way.
- HTTP/2: `http2::session` validates trailers (`validate()`: a HEADERS after the head must end the stream; no
  pseudo-headers; the fields as a head's, so a Content-Length in trailers must be well-formed) and checks the body's
  length against Content-Length when they end it; `submit_headers(stream, fields, true)` can send them (its tests do).
  But the server's `on_headers()` takes request trailers only as the end of the body, and the client's
  `on_headers()` response trailers the same ("aren't passed on"); `http2_finish()` ends a response with an empty DATA
  frame, and the client's `send_body()` a request the same way.
- Nothing in `server_request`, `server_response`, `response_writer`, `client_request` or `client_response` holds them.

**What the RFCs require.** RFC 9110, section 6.5: trailers are "processed and stored separately from the fields in the
header section", and "A recipient MUST NOT merge a trailer field into a header section unless" its definition says how;
"A sender MUST NOT generate a trailer field unless the sender knows the corresponding header field name's definition
permits" it (fields for framing, routing, authentication, request modifiers, response controls or content format
can't be); `Trailer` (section 6.6.2) is what a sender "SHOULD generate" to say which may follow, a hint without a
promise. `TE: trailers` in a request (section 10.1.4) says the client won't drop them; RFC 9110 dropped RFC 7230's rule
that a server mustn't send trailers a client needs without it, leaving "a server SHOULD NOT generate trailer fields that
it believes are necessary for the user agent to receive". RFC 9112, section 7.1.2: over HTTP/1.1 only a chunked message
has them (`last-chunk trailer-section CRLF`), and a sender of TE must list it in Connection (section 7.4). RFC 9113,
section 8.1: a HEADERS frame with END_STREAM after the DATA (or straight after the head), without pseudo-headers;
gRPC's status (`grpc-status`, `grpc-message`) is sent that way, and gRPC clients send `te: trailers`.

**How others do it.**

- Go's server: trailers are declared in `Trailer` before the head is sent (their values set later in `Header()`), or
  set after it under `http.TrailerPrefix` ("Trailer:Foo"); `httpguts.ValidTrailerHeader` refuses names starting `If-`
  and Authorization, Cache-Control, Connection, Content-Encoding, Content-Length, Content-Range, Content-Type, Expect,
  Host, Keep-Alive, Max-Forwards, Pragma, Proxy-Authenticate, Proxy-Authorization, Proxy-Connection, Range, Realm, Te,
  Trailer, Transfer-Encoding and Www-Authenticate (dropped, declared or, over HTTP/2, prefixed). Over HTTP/1.1, trailers
  make it skip the automatic Content-Length, so the body's chunked; they're dropped when it isn't (the handler set a
  Content-Length, HEAD, 204/304, HTTP/1.0). Over HTTP/2 they're kept with a Content-Length, sent as a final HEADERS
  (after the head, with no empty DATA in between, if there's no body); declared ones never set send nothing. Neither
  version looks at the request's `TE`.
- Go's server, receiving: `Request.Trailer`, its keys pre-filled from the request's `Trailer`, filled once the body
  reaches EOF; over HTTP/1.1 every field received is kept (no name check), over HTTP/2 only the declared ones (one that
  fails `ValidTrailerHeader` is a stream error, where a declaration exists).
- Go's client: `Request.Trailer` holds the names to send, whose values may be filled while the body's read; over
  HTTP/1.1 only a chunked request (no ContentLength) sends them; over HTTP/2 any, after the DATA (or an empty DATA with
  END_STREAM, if they come out empty). `Response.Trailer` is filled at the body's EOF (every field, over both versions).
  Neither transport sends `TE: trailers` itself; a caller's is passed on.
- Rust's h2: `SendStream::send_trailers(HeaderMap)` ends the stream; `RecvStream::trailers()` once the DATA has been
  read; http-body 1.0 makes trailers a body's last frame (`Frame::trailers`). hyper's HTTP/1 server sends trailers only
  if the request had `TE: trailers`, and only fields named in the response's `Trailer` (dropping a narrower list:
  authorization, cache-control, content-encoding, content-length, content-range, content-type, host, max-forwards,
  set-cookie, trailer, transfer-encoding, te); received ones are kept unfiltered (within 16 KiB).
- curl passes received trailers to its header callback (`CURLH_TRAILER`) and sends `TE: trailers` only if asked.

**Proposed API** (Decided questions 5 to 12).

```cpp
namespace net::http
{

struct server_request
{
    // ... as now, and:
    // The request's trailer fields, once the body has been read to its end (until then, and for a body without any,
    // empty): kept apart from headers (RFC 9110, section 6.5), and without the fields that can't be trailers (see
    // trailer_allowed()). clear() empties them.
    net::http::headers trailers{};
};

struct server_response
{
    // ... as now, and:
    net::http::headers trailers{};
};

struct response_writer
{
    // trailers are the response's trailer fields (e.g. Server-Timing, or gRPC's grpc-status), sent after the body when
    // it ends (finish(), which the server calls once the handler returns): over HTTP/2, as a final HEADERS frame; over
    // HTTP/1.1, in the last chunk, so only for a body sent without a Content-Length (send(status)) to an HTTP/1.1
    // client, and dropped otherwise. Fields that can't be trailers are dropped (see trailer_allowed()); an
    // invalid name or value (e.g. a line break) fails finish() with std::errc::invalid_argument, and the response is
    // cut short (the connection closed, or the stream reset), so the client doesn't take it as complete. A response
    // without a body ends with its head, unless trailers (or a Trailer field) are set by the time it's sent.
    net::http::headers& trailers() noexcept;
};

struct client_request
{
    // ... as now, and:
    // make_trailers, if set, is called once the body has been read to its end, to fill the request's trailer fields
    // (e.g. a digest of the body, which its reader computed): sent over HTTP/2, and over HTTP/1.1 for a chunked body
    // (one without a Content-Length); not called otherwise. As get_body, it runs on the task sending the request, and
    // is called again for each time the body is (a redirect's 307 or 308).
    std::function<void(headers&)> make_trailers{};
};

struct client_response
{
    // ... as now, and:
    // trailers are the response's trailer fields, once the body has been read to its end (empty until then, and for
    // a response without any), without those that can't be trailers. They're the body's: valid as long as it is.
    [[nodiscard]] const net::http::headers& trailers() const noexcept;
};

// trailer_allowed is whether a field may be sent, or is kept when received, as a trailer: not one RFC 9110 (section
// 6.5.1) rules out, as Go's ValidTrailerHeader decides (framing, routing, authentication, request modifiers, response
// controls and content format: e.g. Content-Length, Host, Authorization, Cache-Control, Content-Type, If-*, Trailer).
[[nodiscard]] bool trailer_allowed(std::string_view name) noexcept;

}
```

**Inside.**

- `chunked_reader` takes a `headers*` to fill (null: checked and dropped, as now), within `max_trailers` and
  `http11::max_fields` lines; `set_request_body()` gives it `&req.trailers`, and the client's `make_response_body()`
  the trailers of the body that owns it (`pooled_body`'s). It fills them before `call_at_end()`'s callback, so the
  watch starts after. `chunked_writer::finish(const headers* trailers)` writes `0\r\n`, the allowed fields and
  `\r\n`, into the buffered writer without a coroutine when they fit (as heads are).
- HTTP/2: the server's `on_headers()` copies a request's trailers (the allowed ones) into the stream's
  `req.trailers` before marking the body's end, under the connection's lock, which the body's reader takes before it
  returns the end, so the handler sees them. `http2_finish()` sends the stream's `resp.trailers` (through
  `append_fields()` and `trailer_allowed()`) as HEADERS with END_STREAM, or an empty DATA with END_STREAM if there are
  none; `http2_send_head()` doesn't end the stream with a bodyless head if trailers (or a `Trailer` field) are set. The
  client's stream keeps a response's trailers for its body (`http2_response_body`), and `send_body()` ends a request
  with `make_trailers`' fields.
- `client_response::trailers()` asks the body: an internal interface (`detail::trailer_source`, a virtual returning
  the trailers) that the client's bodies implement, found with a `dynamic_cast` when it's called (a body replaced by
  the caller has none: a static empty `headers`). So nothing is added to `client_response`, nor allocated.
- The server's 500 for a handler that throws (and `http::timeout`'s 503) clears the trailers along with the headers.
- The session drops nothing itself: what's allowed is HTTP's rule, applied above it, so `fuzz_http2` and the session's
  tests keep seeing every field.
- Middleware sees `req.trailers` once the handler has read the body, and `rw.trailers()` after `next()` returns,
  before the server's `finish()`: so a layer may add one (e.g. `Server-Timing`). `http_metrics` and `http_tracing`
  don't change (a gRPC status is in trailers, with HTTP's 200: recording it is for later).

**Costs.** Without trailers, nothing per request: the `headers` members are inline (a `small_vector` of 8 fields, in
`server_request`, `server_response` and HTTP/2's per-stream state, so one allocation each as now, a few hundred bytes
larger), and are reused per connection (`clear()` keeps their memory, so even trailers don't allocate after the first,
unless long). The client's request trailers cost a `std::function` only when set.

**Fuzzing.** `fuzz_http11`'s transcript includes each request's trailers, so they must decode the same a byte at a
time, and each one accepted must be a valid field (written back unchanged), as headers are. `fuzz_http2` is unchanged
(its scripted server already sends trailers).

**Tests** (no network). Codecs on strings: `chunked_reader` keeps trailers (several, repeated names, `If-` and
Content-Length dropped, over `max_trailers` or `max_fields` failing with `value_too_large`, malformed lines with
`illegal_byte_sequence`), `chunked_writer::finish()` with trailers, and `trailer_allowed()`. The server, over HTTP/1.1
with raw requests and over HTTP/2 with `test::h2_client`: a handler reads a chunked request's trailers after its body;
sends trailers after a chunked body, after a body with a Content-Length (HTTP/2 only; dropped over HTTP/1.1), and with
no body (HTTP/2: HEADERS, then trailers with END_STREAM); disallowed ones dropped; an invalid one cuts the response
short; none to HTTP/1.0 or for HEAD; middleware adds one. The client against the server, over both versions: reads a
response's trailers once the body's done (empty before), sends `make_trailers`' fields (and over HTTP/1.1 only when
chunked), and a redirect calls it again.

### HPACK's dynamic table, for the encoder

**Where things stand.** `hpack::encoder` never indexes (Decided): a static table match is an index (unless the field is
never indexed), anything else a literal without indexing (or never indexed), with the static table's index for its name
if it has one, Huffman coded when that's shorter. `set_max_table_size()` only ever shrinks its table's size (emitting a
size update), as nothing is in it. `append_fields()` marks never indexed what `hpack::sensitive()` names:
authorization, proxy-authorization, cookie and set-cookie. `dynamic_table`, which the decoder uses, holds entries in a
`std::deque` of `std::string`s: an allocation for each field the peer indexes (e.g. every new `:path` from a client that
indexes it, as Go's does), and none of its views stay valid once it changes. `find_static()` goes through the 61 static
entries for each field.

**What RFC 7541 requires.** An encoder's table is the peer decoder's: inserted into and evicted from (oldest first,
until what's left fits, an entry costing its name, its value and 32) exactly as the decoder will; an entry larger than
the table empties it (section 4.4). Its size is at most the peer's SETTINGS_HEADER_TABLE_SIZE, and "An encoder can
choose to use less capacity"; a change is signalled at the start of the next block, and if the size went down and up
between two blocks, "the smallest maximum table size that occurs in that interval MUST be signaled", then the final one
(section 4.2). Never-indexed literals stay so on every hop (section 7.1.3), and section 7.1 is the compression oracle:
an attacker who can add fields to a connection's blocks, and see their size, can confirm guesses at a field's value, so
low-entropy secrets shouldn't be indexed.

**How others do it.**

- Go's encoder indexes everything that fits (`shouldIndex`: `!f.Sensitive && f.Size() <= e.dynTab.maxSize`), name-only
  matches too, with a static exact match first, then a dynamic one, then a static name; its dynamic table is searched
  through two maps (by name, and by name and value) made on the first search. Its `net/http` marks nothing sensitive
  (Authorization and cookies are indexed). Both sides cap the encoder at 4096 bytes (`MaxEncoderHeaderTableSize`)
  whatever the peer offers, and emit the minimum then the final size after a change.
- nghttp2 (`nghttp2_hd.c`, `deflate_nv()`): authorization, and cookies shorter than 20 bytes, are never indexed (as
  Firefox does), and fields the application flags; `:path`, age, content-length, etag, if-modified-since, if-none-match,
  location and set-cookie are literals without indexing (but sent as an index if the table has them exactly); so is an
  entry larger than 3/4 of the table; the rest is indexed. Its encoder's own cap is 4096; its table is searched with a
  128-bucket hash of names, and sequence numbers give indexes without renumbering.
- Rust's h2 does as nghttp2 (its comment: "borrowed from nghttp2"), never indexing cookies whatever their length, and
  without even looking the skipped ones up in the dynamic table; the same 3/4 rule; a 4096 cap.

**Proposed.**

- `dynamic_table`'s storage: one buffer per table, made once (up to twice the table's size), entries' text appended
  into it and moved to its front when it reaches the end (amortized: at most the table's size moved per table's worth
  appended), with a ring of fixed-size descriptors (offset, name and value lengths, a hash of the name and of the value:
  at most size/32 of them). No allocation per entry, for the decoder too. (Decided question 16.)
- The encoder keeps a `dynamic_table` of its own, in step with the peer's decoder, and searches it by going through its
  descriptors (at most 128 for 4096 bytes; the hashes first), as the static table is gone through now: a static exact
  match, then a dynamic exact match, then a static name, then a dynamic name, as Go's.
- What's indexed (Decided questions 13 and 14), decided in `hpack.cpp` from the (lowercase) name:
  - never indexed, as now: `sensitive()`'s names, and whatever the field says (`field::never_indexed`, which re-encodes
    what arrived so, e.g. through a proxy);
  - literals without indexing (as nghttp2's), as they change from message to message, so an entry for them would
    only push others out: `:path`, `age`, `content-length`, `etag`, `if-modified-since`, `if-none-match`, `location`;
    and any field whose entry would be more than 3/4 of the table;
  - everything else is indexed (literal with incremental indexing, then an index while it stays): e.g. `:authority`,
    `user-agent`, `accept`, `content-type`, `server`, `cache-control`, `date` (a new entry a second), `grpc-status`.
- Size: the encoder uses the smaller of the peer's SETTINGS_HEADER_TABLE_SIZE and its own cap (4096, as Go, nghttp2 and
  h2 do: Decided question 15; `session_config::encoder_table_size`, internal), tracks the smallest size since the last
  block, and starts the next one with that, then the final, size update (only if either differs from the size the peer
  knows). `encoder::set_max_table_size()` takes every change (growing too), as the session applies each SETTINGS entry.
- The encoder gets a mode, `hpack::indexing::none` (as now) or `automatic` (the above): the first part builds the table
  with `none` in HTTP/2's sessions, so nothing changes on the wire until the second turns it on.

**Costs.** Per field encoded, a search of at most the table's descriptors (comparing hashes, then the text of a match)
where now there's none: tens of comparisons for a typical head, measured with `bench_request`'s HTTP/2 part (time per
request), against what's saved: the bytes of each repeated field (e.g. a client's `:authority`, `user-agent` and
`accept`, and a server's `content-type`, `server` and `date`: tens of bytes a head, where a request's whole head is
about 100), which the benchmark counts (bytes written per request). No allocations: the buffers are made once per
connection, and the decoder stops allocating per indexed entry (the server's decoder, for clients that index, saves an
allocation or more per request). A connection holds two tables' buffers (8 KiB each at most).

**Fuzzing.** `fuzz_hpack`'s property stays (what one encoder makes of the decoded blocks decodes, by another decoder,
to the same fields, never-indexed included), now with the encoder indexing (`automatic`), and the table size changing
between blocks (each record's control byte, as now, so size updates are exercised); and two more: after each block, the
encoder's table holds exactly the entries the second decoder's does, in order (they're in step), and no never-indexed
field is among them. `fuzz_http2` exercises the session's encoder as before (its blocks decode the same at once or a
byte at a time).

**Tests** (no network). RFC 7541's Appendix C header lists, encoded with indexing, decode to the same fields, leaving
the tables the policy says (e.g. C.3's `:authority` and `custom-key` indexed, its `:path` not); size updates (shrink,
grow, shrink then grow between blocks: two updates, min then final; a SETTINGS above the cap); eviction (an entry larger
than the table empties it; oldest first); never-indexed and without-indexing names stay out of the table; the 3/4 rule;
the storage moving to its front with views of entries kept valid through a decode's callbacks; a second request on a
connection whose head encodes to fewer bytes. Interop by hand: curl and h2load (nghttp2's decoder) against the server,
and the client against Go's server (`test/test_server.go`).

### Priorities (RFC 9218)

**Where things stand.**

- There's no scheduling: a handler's write submits DATA to the session straight away (`stream_writer::write()`), as far
  as the windows allow, while less than 1 MiB of output waits for the writer (`max_output`); the writer takes it all
  and writes it at once. So streams' data goes in the order their handlers wrote it, each as much as its window allows
  (the plan's round-robin writer gave way to this in stage 3: "No queue for the writer, after all").
- RFC 7540's PRIORITY frames, and HEADERS' priority, are checked (a stream can't depend on itself) and ignored;
  PRIORITY_UPDATE (0x10) is ignored as an unknown frame type; SETTINGS_NO_RFC7540_PRIORITIES (0x9) is neither sent nor
  checked; a `priority` field is passed on as any other.

**What RFC 9218 requires.** Priority is two parameters, urgency `u` (0 to 7, highest first, default 3) and incremental
`i` (a boolean, default false), as a Structured Fields Dictionary (RFC 8941, which RFC 9651 obsoletes), in the
`Priority` field (end to end, in requests and responses) or the client's PRIORITY_UPDATE frame (hop by hop, a complete
set: an omitted parameter is its default). Unknown parameters, out-of-range values and values of the wrong type are
ignored; a Dictionary that doesn't parse is ignored whole. A response's `Priority` is the server's view, merged with the
request's (parameters it omits keep the client's: section 8). Scheduling is only RECOMMENDED: higher urgency first;
non-incremental responses of one urgency "one by one, in the order in which those requests were generated" (by stream
id), incremental ones sharing bandwidth; and avoiding starvation (section 10). PRIORITY_UPDATE (section 7.1): on stream
0 (otherwise PROTOCOL_ERROR), never from a server (a client receiving one: PROTOCOL_ERROR), prioritized id 0 is a
PROTOCOL_ERROR, and so is a push stream that's idle; one for a stream not yet open "SHOULD" be kept and applied when it
opens, overriding the request's header (the most recent signal wins), but "The number of streams that have been
prioritized but remain in the 'idle' state plus the number of active streams ... MUST NOT exceed the value of the
SETTINGS_MAX_CONCURRENT_STREAMS", a PROTOCOL_ERROR; one that doesn't parse "MAY" be a connection error.
SETTINGS_NO_RFC7540_PRIORITIES is 0 or 1 (otherwise PROTOCOL_ERROR), in the first SETTINGS, never changed after (a
change MAY be PROTOCOL_ERROR); a server sending 1 says it ignores RFC 7540's signals.

**How others do it.**

- Go 1.27's server schedules by RFC 9218 by default (golang/go#75500; `Server.DisableClientPriority` brings back
  round-robin), with `writesched_priority_rfc9218.go`; it sends SETTINGS_NO_RFC7540_PRIORITIES = 1 (with either
  scheduler) and refuses a value over 1. Its parser is a Structured Fields one (`internal/httpsfv`). Control frames go
  first, then urgencies from 0 to 7; within one, it alternates which of incremental and non-incremental goes first at
  each frame (so they share about half each), incremental streams round-robin a frame at a time, non-incremental ones
  one after another in the order they were opened (a stream with nothing to send skipped). A client that has never
  sent a signal is taken as u=3 and incremental (round-robin, as before), and i=0 is the default only once it has
  shown it knows priorities; behind an intermediary (a request with `Via`, `Forwarded` or `X-Forwarded-For`, which
  sticks for the connection), request headers are ignored (u=3, i=1), PRIORITY_UPDATE still applied. One
  PRIORITY_UPDATE for a stream not yet open is kept (the latest); one that doesn't parse is a stream error. The client
  sends neither the header nor the frame.
- nghttp2 (which removed RFC 7540's priorities in 1.65): eight heaps, one per urgency; within one, ordered by a cycle
  and the stream's sequence, so non-incremental streams go one at a time in the order they were made and incremental
  ones round-robin weighted by bytes. A PRIORITY_UPDATE for an idle stream opens a placeholder stream, counted against
  MAX_CONCURRENT_STREAMS (past it, a connection error), and every PRIORITY_UPDATE counts against its glitch limit; a
  value that doesn't parse is ignored. A server can override a stream's priority (nghttpx does, from a backend's
  response `priority`).
- h2o: RFC 9218 over HTTP/3 (non-incremental streams and incremental ones' first emission in stream-id order, then
  incremental ones round-robin; a PRIORITY_UPDATE received before the request wins over its header); over HTTP/2 the
  RFC 7540 tree, adjusted by a response's `priority`. Rust's h2 has no RFC 9218 support. curl sends no priority signals.
- Browsers (from memory, unverified): Chrome sends `priority` request headers (e.g. `u=0, i` for the document, `u=0`
  for CSS), and PRIORITY_UPDATE only over HTTP/3; Firefox sends both.

**Proposed API.**

```cpp
namespace net::http
{

// http/priority.hpp: an HTTP response's priority (RFC 9218): what a request's Priority field asks for, and what a
// response's may change.
struct priority
{
    std::uint8_t urgency     = 3;     // 0 (most urgent) to 7 (background)
    bool         incremental = false; // whether the client uses the response as it arrives

    bool operator==(const priority&) const = default;
};

// parse_priority reads a Priority field's value (its lines joined with commas) over base: the parameters it has, valid,
// replace base's (others are ignored), and a value that isn't a Structured Fields Dictionary leaves base as it is.
[[nodiscard]] priority parse_priority(std::string_view value, priority base = {}) noexcept;

// to_string formats a priority as a Priority field's value, leaving out defaults: "" for {3, false}, "u=5, i".
[[nodiscard]] std::string to_string(const priority& p);

struct server_config
{
    // ... as now, and:
    // http2_priorities is whether an HTTP/2 connection sends responses by their priority (RFC 9218): as clients ask
    // (the Priority field, and PRIORITY_UPDATE frames) and handlers say (a Priority field in the response). Off, every
    // response shares the connection alike (Go's DisableClientPriority).
    bool http2_priorities = true; // (Decided question 18)
};

}
```

A handler sets its response's priority with the field (`rw.headers().set("Priority", to_string({.urgency = 7}))`,
e.g. for a bulk download), which is also sent to the client; a client caller sets a request's the same way (Decided
question 24). Middleware sees it as any field.

**Inside.**

- `http/structured_fields.hpp` (internal, as `hpack.hpp`): RFC 9651's parsing of a Dictionary, to the end of the
  value: every item type (Integer, Decimal, String, Token, Byte Sequence, Boolean, Date, Display String), inner lists
  and parameters, so that a value with unknown members still parses and one that's malformed doesn't (Decided question
  17); members reported to a callback as views, nothing allocated. `parse_priority()` takes the last `u` and `i` (as a
  Dictionary does: a later key replaces an earlier one), an Integer from 0 to 7 and a Boolean (a bare `i` is true).
- The session (`http2.hpp`, `http2_session.hpp`): `frame_type::priority_update`, `setting_id::no_rfc7540_priorities` and
  `settings::no_rfc7540_priorities`. Both sides send it as 1 in their first SETTINGS (neither sends RFC 7540's signals;
  the server ignores them); a value other than 0 or 1, or a change, is PROTOCOL_ERROR. A server's session takes
  PRIORITY_UPDATE: a non-zero stream id, or prioritized id 0, or an even one (a push stream, never opened), is
  PROTOCOL_ERROR; a payload under 4 bytes FRAME_SIZE_ERROR; it parses the value (so what it keeps is two bytes, not the
  frame's up to 16 KiB) and, for an open stream, reports `session_events::on_priority_update(stream, priority)`; for a
  stream not yet open, keeps the latest (as a stream id and a `priority`, at most MAX_CONCURRENT_STREAMS of them with
  the open streams, past which it's PROTOCOL_ERROR) and reports it just after the stream's head, so it overrides the
  request's field; for a closed one, or a value that doesn't parse, ignores it, as a glitch (Decided question 22). A
  client's session takes PRIORITY_UPDATE as PROTOCOL_ERROR. (A PRIORITY_UPDATE carries a complete set:
  `parse_priority()` over the default, not over what the stream had.)
- The server's connection: each stream's priority, from the request's `priority` lines (`parse_priority()` over the
  default; Decided question 19 for what the default is), then PRIORITY_UPDATEs (over the default), then the response's
  `priority` when its head is sent (over the stream's, as RFC 9218's merging: Decided question 23). Off
  (`http2_priorities = false`), every stream is u=3 and incremental.
- The scheduler (`http/http2_scheduler.hpp`, internal, without I/O, so it's tested alone): eight urgencies, each with
  its non-incremental streams in stream-id order and its incremental ones in a ring; `next()` takes the most urgent
  level with a ready stream, alternating within it between the two kinds (Go's), the incremental ring moving on a frame
  at a time, the non-incremental stream staying first until it has nothing to send (Decided question 20); a stream is
  ready while it has data waiting and window to send it; `update(stream, priority)` moves it (to the end of its new
  ring).
- Writing (`http2_connection_base`; the server's only: the client's request bodies go as now, as Go's): uncontended, as
  now (Decided question 21): a write submits DATA to the session at once while less than `max_ahead` (64 KiB) of DATA is
  waiting for the writer and no stream is waiting in the scheduler, so a connection with one response at a time (e.g.
  `bench_request`'s) pays nothing. Otherwise the stream joins the scheduler with the span it's writing (the handler's
  buffer, valid while its write is awaited) and waits; the writer, each time it has written, takes the session's output
  (control frames and heads, in order, as now: heads are small, and HPACK's order must be kept), then fills it from the
  scheduler, a frame (at most the peer's MAX_FRAME_SIZE, and the windows) at a time, up to `max_ahead`, waking each
  stream whose span is all sent; a stream that fails is removed (under the lock, so the writer never touches its
  span). As a write returns only once its data is framed, a stream's end (empty DATA, or trailers) still follows its
  data. What's in the socket's send buffer is past reordering (TCP_NOTSENT_LOWAT could bound it: later).
- `max_output` (1 MiB) stays as the bound on control frames and heads queued.

**Costs.** None for a connection whose responses don't contend: the scheduler is only touched once `max_ahead` of DATA
waits. Contended: a scheduler step per frame (a few comparisons of eight levels), a wakeup per write completed (where
now a write returns once its data's queued), and smaller writes to the socket (64 KiB of DATA a round, where now up to
1 MiB), which `bench_request` measures with several streams at once over a socket (a new case, beside the one stream
at a time it has) and h2load by hand. Per stream, 2 bytes of priority and the scheduler's links; per connection, the
PRIORITY_UPDATEs kept for idle streams (at most MAX_CONCURRENT_STREAMS). Parsing a request's `priority` costs nothing
when it has none.

**Fuzzing.** `fuzz_http2`'s session receives PRIORITY_UPDATE and the new setting from its raw frames (the property,
the same at once or a byte at a time, holds; its recorder logs `on_priority_update`). A new `fuzz_structured_fields`:
any input parses without crashing, and `parse_priority()` over two bases agrees on what the value sets (a parameter it
has is the same over either); and, for a Dictionary that parses, serialized again (by a test serializer of RFC 9651's
canonical form) parses to the same members.

**Tests** (no network). `parse_priority()` and `to_string()` (RFC 9218's examples, unknown keys, out-of-range and
wrongly typed values ignored, malformed Dictionaries ignored whole, several lines). The session on bytes:
PRIORITY_UPDATE reported for open streams, kept for idle ones and applied after their head, the limit on idle ones, each
error (non-zero stream, id 0, even id, short payload, sent to a client), the setting sent and checked. The scheduler
alone: urgency order, non-incremental by id, incremental round-robin, the alternation, blocked streams skipped, updates
moving streams. Over a socket with `test::h2_client` (its window small, so streams contend): a u=7 bulk response doesn't
hold up a u=0 one started after it (its DATA arrives before the bulk's next frames), non-incremental responses at one
urgency finish in stream-id order, incremental ones interleave, a PRIORITY_UPDATE sent mid-response reorders, a
handler's response `Priority` lowers its stream, `http2_priorities = false` interleaves all; and a single stream's
throughput isn't changed (by frame count and order).

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
5. **Decided (2026-10-20), in ten pull requests, in this order** (see "Stage 5"; each mergeable and tested alone):
   1. **Interim responses, the server.** Done (2026-10-20; see Learned in stage 5). HTTP/1.1 flushes a 1xx at once;
      none to HTTP/1.0; `send(101)` refused; 100 Continue sent by itself (and 417); `status::EARLY_HINTS`; tests over
      both versions; the guide. First: small, a fix (Early Hints don't work over HTTP/1.1 now), and independent of the
      rest.
   2. **Interim responses, the client.** Done (2026-10-20; see Learned in stage 5). `client_request::on_informational`,
      over both versions, and HTTP/2's bound on interim heads. Split from 1 as it's an API of its own (Decided question
      4), tested against 1's server.
   3. **Trailers in the codecs.** Done (2026-10-20; see Learned in stage 5). `chunked_reader` keeping a trailer
      section, `chunked_writer::finish(trailers)`, `trailer_allowed()`, and `fuzz_http11`'s transcript. Split out as
      it's the part without I/O, tested on strings, which both sides' parts build on.
   4. **Trailers on the server.** `server_request::trailers`, `response_writer::trailers()`, over HTTP/1.1 and HTTP/2
      (whose session already handles them); tests with raw requests and `test::h2_client`. One side, both versions: the
      API is the same over either, and the server's tests don't need the client's part.
   5. **Trailers in the client.** `client_response::trailers()` and `client_request::make_trailers`, over both
      versions; tested against 4's server, so after it.
   6. **HPACK's table, built.** `dynamic_table`'s storage (no allocation per entry: the decoder gains at once), the
      encoder's table, search, eviction and size updates, and `hpack::indexing`, with HTTP/2's sessions on `none`, so
      nothing changes on the wire; `fuzz_hpack`'s new properties (with `automatic`). Split from 7 so the mechanics are
      proved on their own, by the fuzzer, before anything depends on them.
   7. **HPACK's indexing, on.** The policy (Decided questions 13 and 14), the encoder's cap (`encoder_table_size`), the
      sessions indexing; `bench_request`'s bytes per request; curl and h2load by hand. A wire change, so its own pull
      request, to measure and to revert alone.
   8. **Structured Fields and `http::priority`.** RFC 9651's Dictionary parsing, `parse_priority()`, `to_string()`,
      `fuzz_structured_fields`. Without I/O, and nothing uses it yet: split out so the parser is reviewed (and fuzzed)
      on its own.
   9. **Priority signals.** The session's PRIORITY_UPDATE and SETTINGS_NO_RFC7540_PRIORITIES, `on_priority_update`, and
      each server stream's priority (the request's field, updates, the response's field), recorded but not yet acted
      on; tests on bytes. Split from 10 as the protocol's rules are tested on bytes, apart from the writer.
   10. **Scheduling by priority.** The scheduler, the connection's contended path, `server_config::http2_priorities`;
      tests over a socket, and `bench_request` with streams that contend. Last: it changes the writer every HTTP/2
      response goes through, so it comes once the rest is in, and alone.

   Trailers come before HPACK and priorities as they're what a user needs (gRPC); HPACK's two parts, internal, before
   priorities' three, the largest change.

   Still later: connection coalescing and the ORIGIN frame (RFC 8336), which mostly browsers use; extended CONNECT (RFC
   8441), which needs a plan for WebSockets first; the client's side of `Expect: 100-continue` (waiting for 100 before
   sending a body); reprioritizing a request from the client (PRIORITY_UPDATE, needing a handle on a request in flight);
   scheduling the client's request bodies; and TCP_NOTSENT_LOWAT, to keep what's past the scheduler small.

## Decided questions (stage 5)

All decided as recommended (2026-10-20); the options are kept for the reasoning.

1. **Interim responses to HTTP/1.x clients.** (a) Sent to HTTP/1.1 clients (flushed at once), never to HTTP/1.0 ones
   (dropped, as RFC 9110 requires); (b) Go's: to both; (c) h2o's: none to HTTP/1.x by default, an option to send them.
   **Recommended: (a)**: what the RFCs say; RFC 8297's warning is about clients that mistake a 1xx for the final
   response, which HTTP/1.1's rule (RFC 9110: a client MUST parse them) has long ruled out; (c) would make Early Hints
   HTTP/2-only for a risk no current client has.
2. **`send(101)`.** (a) Refused (`std::system_error`, `not_supported`) over both versions, until something can switch
   protocols (extended CONNECT, or an Upgrade API); (b) left as it is. **Recommended: (a)**: as an interim response it
   makes an HTTP/1.1 client think the connection switched, and over HTTP/2 it's malformed.
3. **100 Continue.** (a) The server sends it by itself, when the handler first reads the body of a request that expects
   it (unless the handler sent its own 100 or the final response first: an interim response such as a 103 doesn't stop
   it), over both versions, with 417 for other expectations (Go's); (b) left to handlers (they can
   `send(status::CONTINUE)` now, but none will think to); (c) later, with the client's side. **Recommended: (a)**: RFC
   9110 requires an origin server to answer the expectation, and without it curl's uploads (from memory: over 1 MiB)
   wait a second each; it's small, and shares part 1's flushing. (Corrected 2026-10-20: this said "and nothing's been
   sent", which stopped the 100 after a 103 too; part 1 found that isn't Go's rule, and the user chose Go's.)
4. **How interim responses reach a client caller.** (a) Skipped, as now (documented); (b) a callback,
   `client_request::on_informational(status, const headers&)`, for each before the final response (Go's
   `Got1xxResponse`, hyper's `on_informational`); (c) collected in `client_response` (a vector of heads).
   **Recommended: (b)**: costs nothing unset, serves a proxy (which must forward them) and a caller acting on Early
   Hints as they come, which (c) can't (it has them only with the final response, too late for hints), and (c) would
   allocate for every response that has them.
5. **Sending trailers from a handler.** (a) `response_writer::trailers()`, sent by `finish()`, with no declaration
   needed (a `Trailer` field is the handler's to add, as RFC 9110 suggests); (b) only fields declared in the head's
   `Trailer` are sent (hyper's; Go's declared mode); (c) the trailers passed to `finish()`. **Recommended: (a)**: one
   rule over both versions, the headers' shape, and middleware can add to them after `next()`; RFC 9110 has a sender
   SHOULD declare, not must, and (b) drops what a handler forgot to declare, silently; (c) makes the server's own
   `finish()` (after the handler) unable to send them.
6. **Trailers and a Content-Length, over HTTP/1.1.** (a) Dropped, as there's no last chunk to carry them (Go's; HTTP/2
   sends them either way); (b) a `Trailer` field in the head switches `send(status, length)` to chunked over HTTP/1.1
   (the length still limiting the body, but not sent); (c) `send(status, length)` throws if trailers are set.
   **Recommended: (a)**: the handler asked for a Content-Length, which (b) wouldn't send; a handler that has trailers
   to send uses `send(status)`, which the guide says.
7. **`TE: trailers` on the server.** (a) Trailers are sent whatever the request's TE (Go's; RFC 9110 dropped RFC 7230's
   condition); (b) only to requests with `TE: trailers` (hyper's HTTP/1). **Recommended: (a)**: the clients that need
   trailers (gRPC's) send it anyway, and the others drop what they don't read.
8. **`TE: trailers` from the client.** (a) Not sent unless the caller sets it (Go's, curl's); over HTTP/1.1 the encoder
   then adds `TE` to `Connection`, as RFC 9110 (section 10.1.4) requires of a sender of TE; (b) always sent over HTTP/2.
   **Recommended: (a)**: it promises the caller won't drop trailers, which only the caller knows; a gRPC caller sets it.
9. **What's kept of received trailers.** (a) Every field but those `trailer_allowed()` refuses (dropped, not an error),
   with no declaration needed; (b) only those the message declared in `Trailer` (Go's HTTP/2 server); (c) every field
   (Go's HTTP/1.1, hyper). **Recommended: (a)**: one rule for both versions and sides; a field that can't be a trailer
   is dropped rather than shown to a handler that might take it for the header's (e.g. a Content-Length), and failing
   the request for it would turn a peer's harmless extra into an error. `trailer_allowed()` is Go's list (`If-*` and 21
   names), not hyper's narrower one.
10. **How a client caller reads trailers.** (a) `client_response::trailers()`, a member function asking the body (an
    internal interface, through a `dynamic_cast` when called), empty until the body's end; (b) a
    `std::shared_ptr<const headers>` member, filled at the end (an allocation per response); (c) a `headers` member that
    the body fills through a pointer, which `client_response`'s moves re-point. **Recommended: (a)**: nothing added to a
    response that has none, and nothing to keep in step as responses move; the trailers live as long as the body that
    read them.
11. **How a client caller sends trailers.** (a) `client_request::make_trailers`, a callback filling them once the body's
    been read to its end; (b) a `headers` member, fixed before sending; (c) none: receiving only (gRPC's requests have
    none). **Recommended: (a)**: trailers are what's known only after the body (a digest, as the body's reader computes
    it), which (b) can't hold, as `send()` moves the request away from whoever could fill it; it's the user's chosen
    scope, and costs nothing unset.
12. **A trailer that can't be sent (an invalid name or value).** (a) `finish()` fails with `invalid_argument` and the
    response is cut short (HTTP/1.1 closes without the last chunk; HTTP/2 resets with INTERNAL_ERROR), logged; (b)
    those fields dropped, the rest sent. **Recommended: (a)**: as a body shorter than its Content-Length is handled, so
    the client never takes a response missing part of itself for complete; a field with a line break is a handler's
    bug, not something to send around.
13. **What the encoder indexes.** (a) nghttp2's: everything but never-indexed fields (`sensitive()`'s, and those that
    arrived so), the fields that change per message (`:path`, `age`, `content-length`, `etag`, `if-modified-since`,
    `if-none-match`, `location`: literals without indexing) and entries over 3/4 of the table; (b) Go's: everything that
    fits but never-indexed ones; (c) only a list of fields known to repeat (`:authority`, `user-agent`, `accept`,
    `content-type`, `server`, ...). **Recommended: (a)**: (b) fills the table with entries never used again (a new
    `:path` each request pushes out what repeats); (c) misses an application's own repeated fields (e.g.
    `grpc-status`, `x-ratelimit-limit`); nghttp2's list is what h2 also chose.
14. **Credentials and cookies.** (a) Never indexed, as now (`authorization`, `proxy-authorization`, `cookie`,
    `set-cookie`; h2 does so too, for cookies); (b) nghttp2's: cookies of 20 bytes or more indexed (high-entropy enough
    not to be guessed); (c) Go's: all indexed. **Recommended: (a)**: the compression oracle is about exactly these, and
    a client whose requests carry several users' credentials (e.g. a gateway) would expose them under (b) or (c);
    what it costs is resending a bearer token (often 1 KiB) per request, which (b) would save only for cookies.
15. **The encoder's table size.** (a) The smaller of the peer's SETTINGS_HEADER_TABLE_SIZE and 4096 (Go's, nghttp2's
    and h2's cap), internal (`session_config::encoder_table_size`); (b) whatever the peer offers; (c) (a), with the cap
    an option in `server_config` and `client_options`. **Recommended: (a)**: 4096 is what everyone settled on, a
    larger table costs memory per connection (and a longer search), and nothing has asked for more.
16. **The table's storage.** (a) One buffer per table, entries appended and moved to the front at its end, with fixed
    descriptors (hashes included) searched in order: no allocation per entry; (b) as now (a string per entry), with hash
    maps to search it (Go's, nghttp2's). **Recommended: (a)**: at most 128 entries for 4096 bytes, so a search through
    descriptors comparing hashes is as quick as a map, without its allocations; and it removes the decoder's
    allocation per indexed field, which clients that index (Go's, curl's, browsers') cost the server now.
17. **Structured Fields.** (a) RFC 9651's Dictionary parsing, whole (every item type, inner lists, parameters), in an
    internal header (Go's `httpsfv`, nghttp2's `sfparse`); (b) h2o's lenient reading of `u` and `i` tokens only.
    **Recommended: (a)**: RFC 9218 ignores a Dictionary that doesn't parse, which only a real parser can tell from one
    with members it doesn't know; it's fuzzable, and other fields (e.g. `Cache-Status`, client hints) can use it later.
18. **Priorities on by default.** (a) On, with `server_config::http2_priorities = false` to share alike (Go 1.27's,
    with `DisableClientPriority`); (b) off unless asked for. **Recommended: (a)**: with Go's default (19's), clients
    that send nothing get a fair share (round-robin), near what they get now, and browsers' signals are used.
19. **The default for a request without signals.** (a) Go's: u=3 incremental (round-robin) until the connection has
    sent a signal (a `priority` field or a PRIORITY_UPDATE), then RFC 9218's u=3 non-incremental; and behind an
    intermediary (a request with `Via`, `Forwarded` or `X-Forwarded-For`, sticky for the connection), request fields
    ignored (PRIORITY_UPDATE still applied); (b) RFC 9218's defaults always (u=3, i=0: one at a time, by stream id);
    (c) (a) without the intermediary rule. **Recommended: (a)**: (b) makes a long download hold up every API response
    behind it on clients that never asked for that order (the starvation RFC 9218, section 10, says to avoid); the
    intermediary rule is RFC 9218's section 13.1 (a proxy merging clients' requests shouldn't let one client's signals
    order another's), and Go's.
20. **Order within an urgency.** (a) Go's: incremental and non-incremental take turns a frame at a time, incremental
    ones round-robin, non-incremental ones one at a time by stream id; (b) h2o's: non-incremental ones first, then
    incremental ones; (c) nghttp2's: by bytes sent (a cycle), non-incremental by creation. **Recommended: (a)**: the
    simplest that avoids section 10's starvation examples (a large non-incremental response holding up an incremental
    one of the same urgency, and the reverse), which (b) has; (c)'s weighting by bytes buys little over frames of the
    same size.
21. **When the scheduler applies.** (a) Only under contention: a write goes to the session at once while less than
    `max_ahead` (64 KiB) of DATA waits for the writer and no stream waits in the scheduler, and otherwise waits its
    turn;
    (b) always (Go: every frame through the scheduler). **Recommended: (a)**: scheduling only changes anything when
    streams compete for the socket, and (b) would add a handoff to the writer to every write, which `bench_request`'s
    one stream at a time would show; `max_ahead` (how much the scheduler can't reorder) is to be tuned with h2load.
22. **PRIORITY_UPDATE's edges.** (a) For a stream not yet open, the latest kept (at most MAX_CONCURRENT_STREAMS with the
    open streams, RFC 9218's bound, past which it's PROTOCOL_ERROR) and applied after its head; a value that doesn't
    parse, or one for a closed stream, ignored and counted as a glitch; (b) Go's: one kept for the connection, a value
    that doesn't parse a stream error; (c) nghttp2's: every PRIORITY_UPDATE a glitch. **Recommended: (a)**: what the RFC
    asks (SHOULD keep, bounded as it says), without a new error for a malformed hint, and with the glitch budget
    (MadeYouReset's) bounding a flood of useless ones without counting a browser's real reprioritizing.
23. **A response's `Priority`.** (a) Merged into the stream's when the head's sent (RFC 9218, section 8: its parameters
    replace the client's, others kept; nghttp2/nghttpx and h2o do); (b) ignored (Go's). **Recommended: (a)**: it's how
    a handler marks a background download, or raises a critical resource, with nothing new in the API.
24. **What the client sends.** (a) SETTINGS_NO_RFC7540_PRIORITIES = 1 and nothing more: callers set a request's
    `Priority` field themselves (`to_string(priority{...})`), and no PRIORITY_UPDATE; (b) a `client_request::priority`
    member, sent as the field; (c) also PRIORITY_UPDATE, to change a request in flight. **Recommended: (a)**: the field
    is an ordinary end-to-end header, which (b) would only spell differently; Go's and curl's clients send nothing; (c)
    needs a handle on a request in flight, for which nothing has asked (Still later).
25. **The parts' order.** (a) As in Stages: interim responses, trailers, HPACK, priorities; (b) HPACK first (internal,
    independent), then trailers; (c) priorities before HPACK. **Recommended: (a)**: the fix first, then what users need
    (trailers, for gRPC), then the internal gain, and the riskiest change to the writer last; HPACK and priorities don't
    depend on each other, so (c) is fine too if browsers matter sooner.

## Out of scope

Server push (browsers dropped it), `Upgrade: h2c` (RFC 9113 dropped it), RFC 7540's priority tree (deprecated; its
frames are parsed and ignored), and HTTP/3 (parked: [http3.md](http3.md)).
