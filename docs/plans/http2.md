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
`trailer_allowed()`), done (2026-10-20); its fourth, trailers on the server (`server_request::trailers`,
`response_writer::trailers()`), done (2026-10-20); its fifth, trailers in the client (`client_response::trailers()`,
`client_request::make_trailers`), done (2026-10-20); then trailers held in `trailer_fields`, 2 fields inline
rather than `headers`' 8 (2026-10-20; see "Trailers' size (after part 5)"); its sixth, HPACK's dynamic table built
(its storage, the encoder's table and `hpack::indexing`, with the sessions still on `none`), done (2026-10-20); and its
seventh, HPACK's indexing on in both sides' sessions (`indexing::automatic`, the encoder held to 4096 by
`session_config::encoder_table_size`), done (2026-10-21); and its eighth, Structured Fields and `http::priority`
(`http/structured_fields.hpp`, `http/priority.hpp`, `fuzz_structured_fields`), done (2026-10-21); and its ninth,
priority signals (PRIORITY_UPDATE and SETTINGS_NO_RFC7540_PRIORITIES in the session, each server stream's priority,
`server_config::http2_priorities`), done (2026-10-21); and its tenth, scheduling the server's responses by priority
(`http/http2_scheduler.hpp`, the connection's contended path, `fuzz_http2_scheduler`), done (2026-10-21): stage 5 is
done. Stage 6 decided (2026-10-21): finishing HTTP/2 (TCP_NOTSENT_LOWAT on the server's connections, ending streams
with their last DATA, 431 for a header list over the limit, `Expect: 100-continue` in the client, scheduling and
reprioritizing the client's requests, and connection coalescing with the ORIGIN frame), in twelve pull requests (see
"Stage 6"); its questions (at the end) all as recommended. Its first part, ending a body of known length with its last
DATA frame on both sides (`http2_stream_base::data_left`), done (2026-10-21; see Learned in stage 6); its second,
431 for a request's header list over the limit (`session_events::on_header_list_too_large()`, answered by the server's
`refuse()`), done (2026-10-21); and its third, TCP_NOTSENT_LOWAT's part in the socket (`tcp_socket::set_notsent_lowat()`
and `socket::wait_writable()`), done (2026-10-21); and its fourth, TCP_NOTSENT_LOWAT on the server's HTTP/2
connections (`server_config::http2_notsent_lowat`, the writer waiting for the socket before each round, and
`tls::stream::wait_writable()`), done (2026-10-21); and its fifth, `Expect: 100-continue` in the client over HTTP/1.1
(`client_options::expect_continue_timeout`, the encoder's head apart from the body, `response_decode()`'s
`stop_at_interim`, and 417 sent again without the expectation), done (2026-10-21); and its sixth, `Expect: 100-continue`
in the client over HTTP/2 (the stream's wait, `continue_deadline` checked by the connection's timer, and 417 sent again
on the same connection), done (2026-10-21); and its seventh, the client's request bodies through the connection's
scheduler (one write path for both sides, every client stream u=3 incremental), done (2026-10-21); and its eighth, the
client's request bodies by their requests' `Priority` fields, and TCP_NOTSENT_LOWAT on the client's HTTP/2 connections
(`client_options::http2_notsent_lowat`), done (2026-10-21).

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
- [RFC 8336](https://www.rfc-editor.org/rfc/rfc8336.html), the ORIGIN frame (which origins a connection serves, for
  coalescing). Stage 6.
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

### Part 4: trailers on the server (done 2026-10-20)

- `server_request::trailers` and `server_response::trailers` (with `response_writer::trailers()`), as proposed:
  `headers` members, inline. `server_request::clear()` empties the request's; the server's `start_response()` (and
  `respond_and_close()`) empty the response's, as HTTP/1.1's response is the connection's, reused per request.
- HTTP/1.1, receiving: `http11::keep_body_trailers(req)` (new, beside `call_at_body_end()` and
  `call_at_body_start()`, told apart by `typeid` as they are) has a chunked body keep its trailer section in
  `req.trailers`; the server calls it just before `call_at_body_end()`, so they're filled before the end callback and
  the watch. Not in `set_request_body()`: the by-value `request_decode()` moves the request it decoded into, which
  would leave the body pointing into the moved-from one.
- HTTP/2, receiving, differs from the plan: `on_headers()` doesn't add to `req.trailers`, but keeps the allowed ones in
  the stream (`http2_stream_base::received_trailers`, a `unique_ptr<headers>` made only when a message has some), and
  the body's reader moves them to the stream's `trailers_to` (`&req.trailers`) as it returns the end
  (`hand_over_trailers()`, under the lock). The handler runs on another thread than the connection's reader, so with
  the plan's way a handler that looked at `req.trailers` before the body's end (or middleware after `next()`, the
  body unread) would race with the reader adding to them, and they'd appear whenever they arrived, not only at the
  end as the API says. In the base, as part 5's client needs the same for a response's body; on the heap, so streams
  without trailers (the client's too) don't grow by a `headers` (680 bytes with libc++).
- HTTP/1.1, sending: `finish()` passes `&resp->trailers` to `chunked_writer::finish()` when the body's chunked;
  otherwise they're dropped without being looked at (a Content-Length, HTTP/1.0). For HEAD the server clears them
  before `finish()`: the chunked writer writes into the HEAD's discard writer, but would still check them, so an
  invalid one would close the connection for nothing; over HTTP/2 a HEAD's head ends the stream, so both versions
  drop them unchecked alike.
- HTTP/2, sending: `append_fields()`'s `bool request` became `field_section` (`response`, `request`, `trailers`); the
  trailers' leaves out what `trailer_allowed()` refuses (besides the connection-specific fields), and checks every
  field with `valid_fields()`, those it leaves out too, as `chunked_writer::finish()` does. `http2_finish()` appends
  them before it marks the stream ended, so one that can't be sent leaves the stream to be reset; then HEADERS with
  END_STREAM, or an empty DATA if none are left. `http2_send_head()` doesn't end a bodyless head if
  `resp.trailers` isn't empty or `Trailer` is among the headers (a 1xx never ends it, and HEAD's always does).
- An invalid trailer: `finish()` fails with `invalid_argument`, logged at warn ("handler for GET /x set a trailer that
  can't be sent"; other failures to finish stay at debug). HTTP/1.1 then closes without the last chunk (what was
  buffered of the body is flushed first); HTTP/2's `respond_after()` returns false, and the stream's reset with
  INTERNAL_ERROR (CANCEL if the handler was stopped).
- The 500 for a handler that throws clears the trailers with the headers (HTTP/2's in `respond_after()`, HTTP/1.1's
  in `respond_and_close()`, whose 500 has a Content-Length and so would drop them anyway). `http::timeout`'s 503 clears
  only the trailers: it keeps the rest of the chain's headers, as before.
- `fuzz_http11` now decodes into one `server_request` reused for each request, as the server does (the in-place
  `request_decode()`, then `keep_body_trailers()`), and reads `req.trailers`, instead of the `dynamic_cast`; it also
  checks that a request starts without the last one's. Ran for 110 s from two seeds with trailers (106,706 runs);
  `fuzz_http2` (unchanged) for 110 s (59,072 runs).
- Tests (`test/http/server_trailers.cpp`, `[trailers]`): over HTTP/1.1 with raw requests, a handler reads a chunked
  request's trailers after its body (a disallowed one dropped, a repeated name one field); trailers in the last chunk
  (disallowed ones dropped), none after a Content-Length, without a body, to HTTP/1.0 or for HEAD; an invalid one
  closes the connection without the last chunk; none with the 500; the connection reused after a response with
  trailers, the next request having none; middleware adding one. Over HTTP/2 with `test::h2_client` (which now records
  a response's trailers, its DATA frames and whether its head ended the stream, and sends trailers): a request's
  trailers after its body, and straight after its head; trailers after a chunked body, after a Content-Length, straight
  after a bodyless head (no DATA), a declared `Trailer` with none (an empty DATA ends it), none for HEAD, the reset for
  an invalid one, none with the 500; middleware adding one. And `append_fields()`'s `trailers` section on its own.
- Sizes (libc++): `headers` is 680 bytes, so `server_request` grew from 1096 to 1776, `server_response` from 704 to
  1384, and the HTTP/2 server's stream by 1376 (those two, `received_trailers` and `trailers_to`); `http2_stream_base`
  from 272 to 288. Each is still one allocation (per connection, or per stream), as the plan expected, if larger than
  its "few hundred bytes".
- Measured (`bench_request`, Release, five alternating runs of the binaries before and after, best of each):
  allocations unchanged everywhere (1.4 per request pipelined, 4.2 one per read, 3.0 over a socket, 2.0 to a handler
  that waits, 52.0 over HTTP/2, and the middleware runs' 2.4, 1.4, 2.4, 3.4, 2.4 and 4.0). Times: pipelined 1122 ns
  before and 1147 after (the runs' spread 1122 to 1162 before, 1147 to 1175 after), one per read 1332 and 1324, over a
  socket 27.6 and 28.8 us (27.6 to 29.8 before, 28.8 to 29.1 after), to a handler that waits 35.7 and 36.3, over HTTP/2
  80.4 and 80.3, and the middleware runs within 1% either way. What a request without trailers now does more is two
  `clear()`s of empty `headers`, a `typeid` check, and a test for HEAD; the pipelined run's 2% is at the edge of its
  spread, and taken for noise or layout.

### Part 5: trailers in the client (done 2026-10-20)

- `client_response::trailers()` and `client_request::make_trailers` as proposed. `detail::trailer_source` is in
  `response.hpp` (a pure virtual `trailers()`, protected special members); `trailers()` finds it with a `dynamic_cast`
  of the body, and returns a static empty `headers` for a body that isn't the client's (or none). The client hands the
  caller its own bodies only: `pooled_body` (HTTP/1.1) and `http2_response_body` (HTTP/2); redirects' `discard()` reads
  the responses it drops, and there's no decompression nor other wrapper, so nothing hides them.
- HTTP/1.1, receiving: `pooled_body` keeps them in a `headers` member (`kept`), as its chunked reader (and buffered
  reader) are dropped once the body's done; `attach()` (which now takes the response) calls
  `http11::keep_body_trailers(const client_response&, headers&)`, a new overload beside the server's, which tells the
  chunked reader apart by `typeid`. HTTP/2, receiving: the client's `on_headers()` keeps a trailers block's allowed
  fields in the stream's `received_trailers` (part 4's, on the heap only for a message that has some), and
  `http2_response_body` points the stream's `trailers_to` at its own `kept`, so the stream reader hands them over as it
  returns the end, on the body's task. Both bodies grow by a `headers` (680 bytes with libc++), each still one
  allocation per response, as before.
- HTTP/1.1, sending: `request_encode()` owns the `chunked_writer`; `copy_chunked()` no longer finishes, and the last
  chunk is written after the body's `try`: by `finish()`, or, with `make_trailers` set, by `finish_trailing()`, a
  coroutine of its own (so only a request that has trailers has their `headers` in a frame), which calls it, then
  `finish(&trailers)`. Called outside the `try`, so whatever it throws (even `std::bad_alloc`) comes out of the task as
  it is. Only for a chunked request: one with a Content-Length doesn't call it, nor one without a body, unless the
  caller asked for chunked (`Transfer-Encoding: chunked`), which is then just the last chunk, with them. An invalid
  trailer fails the encode with `invalid_argument` before the last chunk is written (what was buffered of the request
  isn't flushed, so a short body may not reach the server at all), and the `pooled_body` closes the connection.
- HTTP/2, sending: decided beyond the plan, a request without a body calls it too, as Go's transport does (a request
  with trailers doesn't end the stream with its head): the head goes without END_STREAM when `make_trailers` is set
  (`send()`'s `trailing`), and `send_body()` (reading nothing for a length of 0) ends the request with `end_request()`,
  a plain function (so the `headers` are on the stack, not in `send_body()`'s frame), which calls it and then, under the
  lock, `append_fields(..., field_section::trailers)` into the stream's `names` and `fields` (the head's, already
  encoded), HEADERS with END_STREAM, or an empty DATA if none are left. An invalid one: nothing's sent, and the stream's
  `abandon()`ed (reset with CANCEL, as for a body that fails: the server's INTERNAL_ERROR is for its own responses),
  failing the request with `invalid_argument` as a body failure (the caller's mistake, not retried). An exception from
  `make_trailers` abandons the stream too, then comes out of `send()`.
- Redirects: `make_trailers` stays on a 307 or 308's request (called again, as the body's read again), and is dropped
  by a 301, 302 or 303's, with the body they describe. Retries call it again
  (over HTTP/1.1 only a bodyless request is retried, which calls it only if the caller asked for chunked).
- TE (Decided question 8): nothing sends `TE: trailers` by itself. HTTP/1.1's encoder didn't list a caller's TE in
  Connection; now `request_encode()` adds a `Connection: TE` line when the request has TE and Connection doesn't list
  it (one more field line beside a caller's Connection, which RFC 9110 has recipients combine). HTTP/2's
  `append_fields()` already dropped TE values other than `trailers`, but also dropped `te` altogether when Connection
  named it (as a caller following RFC 9110 for HTTP/1.1 would): it now keeps a request's `te: trailers` whatever
  Connection says (Connection itself is never sent).
- Tests (`test/http/client_trailers.cpp`, `[http][client][trailers]`, against cppnet's server, over both versions):
  a response's trailers once its body's end has been read, empty before and after its first byte, apart from the
  headers, after a chunked body, a Content-Length (HTTP/2 only) and no body (HTTP/2 only); disallowed ones dropped
  (against servers the test plays: raw HTTP/1.1, and `test::h2_client` as a server, as cppnet's server drops them
  itself); `make_trailers`' fields reaching the handler's `req.trailers` (a disallowed one dropped) for a chunked body,
  a Content-Length and no body, called once over HTTP/2 and only for the chunked body over HTTP/1.1; a 307 calling it
  again, a 303 not; an invalid one failing the request with `invalid_argument`, the handler's body read failing (the
  connection closed, or the stream reset), and the client going on; a caller's TE (`trailers` or `gzip`, with and
  without its own `Connection: TE`): over HTTP/1.1 as given with TE in Connection once, over HTTP/2 only `trailers`; a
  body the caller replaced (or removed) having none; and the next responses on a connection (one with other
  trailers, one without) having their own, the first's kept, one connection. And `test/http/http11.cpp`: the
  encoder's last chunk with them (not called with a Content-Length, an invalid one failing without the last chunk),
  `Connection: TE` added unless it's there, and a response's chunked body keeping them through `keep_body_trailers()`.
- Measured (`bench_request`, Release, five alternating runs of the binaries before and after): over HTTP/2 (cppnet's
  client and server) 51.9 to 52.0 allocations a request before, 51.8 to 51.9 after (the same: nothing per request was
  added), 79.9 to 86.9 us before and 81.4 to 85.9 after; over HTTP/1.1 with cppnet's client (prior knowledge off, a
  throwaway change, as in part 2) 28.0 allocations both, 42.6 to 43.1 us before and 42.1 to 45.1 after. The server's
  runs are unchanged in allocations (1.4, 4.2, 3.0, 2.0, and the middleware runs' 2.4, 1.4, 2.4, 3.4, 2.4 and 4.0),
  and their best times within each other's spread (pipelined 1115 ns before, 1127 after; over a socket 29.1 and 28.1
  us). What a request without trailers now does more: a test of `make_trailers`, an empty `headers` made and destroyed
  (HTTP/2's `end_request()`), and over HTTP/1.1 a look for TE among the request's fields.

### Trailers' size (after part 5)

- Parts 3 to 5 held trailers in a `headers`: 680 bytes with libc++ (8 fields of 80 bytes held inline), for something
  almost always empty, or a field or two (gRPC's `grpc-status` and `grpc-message`, `Server-Timing`). The user chose
  (2026-10-20) to hold 2 inline: `headers` became `basic_headers<N>` (`using headers = basic_headers<8>`, unchanged),
  and `trailer_fields = basic_headers<2>` (200 bytes) is the type of every trailers member and API:
  `server_request::trailers`, `server_response::trailers` (`response_writer::trailers()`), `client_response::trailers()`
  (`detail::trailer_source`) and the client's bodies' `kept`, HTTP/2's `received_trailers` and `trailers_to` (the
  hand-over still a move), `chunked_reader::keep_trailers()`, `chunked_writer::finish()`,
  `http11::keep_body_trailers()`'s client overload, and `client_request::make_trailers`. More than 2 still work, and
  allocate, as `headers`' do past 8.
- A template, rather than a capacity-erased base under `util::small_vector` (LLVM's `SmallVectorImpl`): `small_vector`
  is a `std::array` and a `std::vector`, with no base to erase N into, and a template keeps `headers`' code as it was.
  The members stay in `headers.cpp`, explicitly instantiated for 8 and 2 (`extern template` in the header). A field is
  `header_field` whatever N, so what only reads fields takes a `fields_view` (`std::span<const header_field>`, which
  either converts to): `valid_fields()`, `same_fields()` (behind an `operator==` comparing any two), and HTTP/1.1's
  encoder pieces; HTTP/2's `append_fields()`, which looks fields up, is a template instantiated for both.
- Clang's code for `headers` was compared function by function (Release objects, before and after). Two changes were
  needed to keep it: `for_each_element` (behind `get_list()` and `has_token()`) became a free function local to
  `headers.cpp`, as the member template, given lambdas from a class template's members, had external linkage and
  stopped being inlined into its callers; and `begin()`, `end()`, `size()` and `empty()` are defined in the class, so
  a `fields_view` is made without calls (they were out of line before, so the encoder makes fewer calls than it did).
  HTTP/2's `append_fields()` now calls `connection_specific()` rather than inlining it (it has two callers now).
- Sizes (libc++, before and after): `trailer_fields` 680 to 200 bytes; `server_request` 1776 to 1296;
  `server_response` 1384 to 904; the HTTP/2 server's stream 3544 to 2584; `pooled_body` 832 to 352;
  `http2_response_body` 856 to 376.
- Measured (`bench_request`, Release, five alternating runs of the binaries before and after): allocations unchanged
  everywhere (1.4 a request pipelined, 4.2 one per read, 3.0 and 2.0 over a socket, 51.7 to 52.1 over HTTP/2, the
  middleware runs' 2.4, 1.4, 2.4, 3.4, 2.4 and 4.0); over a socket and HTTP/2, times within each other's spread (27.6 to
  29.3 us before, 27.9 to 28.8 after; HTTP/2 80.1 to 85.6 and 80.9 to 84.1). The runs from memory came out 2 to 3%
  slower (pipelined 1116 to 1161 ns before, 1156 to 1198 after): not from the code, it seems, as the same build with
  `trailer_fields` holding 8 (the old sizes) was as fast as before (1119 to 1130 ns, against 1127 to 1170 in those
  runs), and this one nearly so with the bench's coroutine frame padded back to the old offsets (its `server_request`
  and `server_response` are locals of the frame; 1128 to 1181): it's where the frame's locals fall.

### Part 6: HPACK's table, built (done 2026-10-20)

- `dynamic_table`'s storage as Decided question 16 (a), with one change: its two buffers, the fields' text (appended
  oldest first, and moved to the front when a field doesn't fit after the newest) and the ring of fixed-size entries
  (24 bytes with libc++: the text's offset, the name's and value's lengths and hashes), aren't made at their full size
  at once but grow by doubling as fields need them (from 512 bytes and 16 entries), up to twice the maximum size and a
  32nd of it. So a table nothing's added to allocates nothing (an encoder with `indexing::none`, and the decoder of a
  peer that never indexes, such as cppnet's own client), and a table whose maximum size a peer set high (until part 7's
  cap, the encoder's is whatever SETTINGS_HEADER_TABLE_SIZE says, up to 4 GiB) holds only what's put in it. A table
  filled to 4096 bytes grows five or six times, once, over its first few kilobytes of fields. A field's lengths are 32
  bits, so the maximum size is held to 2^32 - 1 (all HPACK's integers and SETTINGS can say anyway).
- What's moved to the front includes the fields the new one is about to evict: `insert()` copies a field that may be
  a view of one of the table's own (the decoder's literal whose name is an index of the dynamic table, as in RFC 7541,
  C.5.2's), so the text is moved (or copied to the larger buffer) first, the views re-pointed if they were into it, the
  field copied after the newest, and only then the oldest evicted. What's kept is at most the maximum size, and the new
  field's text at most the maximum size less 32, so in a buffer of twice the maximum size it always fits; when the
  buffer's smaller, it's grown rather than moved within if what's kept and the new field would fill more than half of
  it. The decoder's promise is unchanged (a field's views are valid during its callback: it's passed on before it's
  inserted), and so is `at()`'s (valid until the table next changes). The hash is a multiply-and-fold over 8 bytes at a
  time (only ever compared within the process); `find()` goes through the entries newest first comparing hashes and
  lengths before text, and gives the newest exact match and the newest with the name.
- The encoder: `encoder(hpack::indexing)`, `none` by default, with its own `dynamic_table` (`table()`, for the tests
  and the fuzzer). `automatic` is Decided questions 13 and 14's policy, decided by the field's name: never indexed for
  `field::never_indexed` and `sensitive()`'s names, which it sends as never-indexed literals whatever the field says
  (rather than as literals without indexing, as `none` sends an unmarked one: so intermediaries don't index them
  either, as nghttp2 does for authorization); literals without indexing for nghttp2's list and for a field over 3/4 of
  the table (`size * 4 > max * 3`; a table of 0 indexes nothing); the rest indexed. Searched in Go's order: a static
  exact match, a dynamic exact match (neither for a never-indexed field), then the name's index, the static table's or
  else the newest dynamic one's. `none` is exactly what was built in stage 1: the same code, with the table never
  searched nor added to, and `sensitive()` not looked at.
- Size updates: the encoder tracks the smallest maximum size since the last block, and starts the next with it if
  it's below both what the peer's decoder knows and the final size (it may have evicted fields there, which the
  decoder must too), then the final size if it differs from what the decoder knows or the smallest was below it. So
  shrinking then growing between blocks is two updates (min, then final), growing then shrinking back to what's known
  is none, and a change that ends where it started without going below is none. `set_max_table_size()` takes each of
  the peer's settings (growing too) with `automatic`; with `none` it keeps stage 1's rule, only ever shrinking (it's
  min'ed with the current size), which is what keeps the sessions' bytes the same: growing would send a size update
  to a peer that offers more than 4096 (as Firefox's 65536), for a table that's empty. Part 7 can drop that rule with
  the cap.
- HTTP/2's sessions still default-construct their encoder (`none`), so nothing on the wire changed. Checked with a
  throwaway differential fuzzer (master's `hpack.cpp`, renamed into another namespace, against the branch's, in one
  binary): random field lists (static-table names and values, sensitive names, never-indexed flags, random text) and
  table size changes encoded by both encoders, byte for byte the same, and random blocks decoded by both decoders, the
  same fields, errors and tables; 1.7 million runs in 300 s, no difference. And the suite's HTTP/2 tests (which check
  some blocks' bytes), `fuzz_http2` and the rest pass unchanged.
- `fuzz_hpack` (its records' control byte now changes the size to 128s of its low 6 bits, and with bit 6 set again to
  4096, so it shrinks then grows, or grows then shrinks, between blocks): what's decoded is re-encoded by an
  `automatic` encoder and by a `none` one, each to its own decoder: the fields must decode the same (with
  `automatic`, never indexed if they were or are `sensitive()`); after each block the `automatic` encoder's table must
  hold the same fields as its decoder's, in order, none of them `sensitive()`, and a field new to it in the block must
  be in the block unmarked (so nothing was indexed from a never-indexed field); the `none` encoder's table stays empty.
  600 s (about 430,000 runs, inputs up to 2 KiB) found nothing; nor did the 10 s ctest run.
- Tests (`[http][hpack]`, with `[encoder][indexing]`, `[encoder][size]`, `[table]`): C.4's requests encoded with
  `automatic` give C.4's bytes exactly (`:authority` and `cache-control` indexed, the rest static indexes) and its
  tables; C.6's responses (a table of 256) decode to the same fields, `location` a literal without indexing and
  `set-cookie` never indexed, the tables as the policy says (159, 201 and 224 bytes); size updates (shrinking, growing,
  shrinking then growing back or past, growing then shrinking back or below, twice, the same, and `none`'s), each
  accepted by a decoder that's had the same settings, and signalled once; a table shrunk to 0 and grown again emptied
  on both sides; eviction oldest first, the encoder's table equal to its decoder's; never-indexed and per-message names
  kept out of the table (the same block twice gives the same bytes), and an indexed field marked never indexed sent as
  a never-indexed literal (with its dynamic name index); the 3/4 rule at its edges; 20,000 operations on a table
  (inserts of new fields and of fields named or valued from the table's own, size changes) against a model, `find()`
  included; a field named or valued from the one it evicts as the text moves over where it was, and named from one as
  the buffer grows (each found by breaking the re-pointing: the moving case wasn't caught by the model test alone, as
  the old bytes were usually still there, nor the growing case, which ASan catches only when it's hit); and a decoder
  given literals named by index 62 for 1000 blocks, its callbacks seeing the right fields as the text moves. A second
  request on a connection encoding to fewer bytes, and a SETTINGS above the cap, are part 7's (the sessions).
- Sizes (libc++): `encoder` 24 to 112 bytes (its table); `decoder` 136 to 160; `http2::session` 672 to 784.
- Measured (`bench_request`, Release, five alternating runs of the binaries before and after): over HTTP/2 (cppnet's
  client and server) 51.8 to 52.0 allocations a request before, 51.7 to 51.9 after, and 81.2 to 86.0 us before, 81.2
  to 85.8 after: unchanged, as cppnet's client doesn't index yet (until part 7), so the server's decoder never inserted
  anything, before or after. Pipelined 1162 to 1202 ns before, 1168 to 1204 after; over a socket 27.8 to 28.9 us
  before, 28.0 to 30.3 after (one run of 30.3; the rest within master's spread). What a client that indexes saves was
  measured apart (a throwaway benchmark, -O2, master's decoder against the branch's in one binary): blocks as Go's
  encoder makes them (a new `:path` of about 40 bytes each request, with incremental indexing, and `:authority`,
  `user-agent` and `accept-encoding` indexes while they last, re-added when the `:path`s push them out): 1.04
  allocations a block before, 0 after, and 106 to 110 ns a block before, 79 to 95 after.

### Part 7: HPACK's indexing, on (done 2026-10-21)

- Both sides' sessions encode with `indexing::automatic` (`http2_session.cpp`: the encoder's constructed so), held to
  `session_config::encoder_table_size` (internal, 4096, Decided question 15 (a)): the session gives the encoder the
  smaller of the cap and the peer's SETTINGS_HEADER_TABLE_SIZE, at construction (the peer's initial 4096) and with
  each SETTINGS. So a peer offering more (Firefox's 65536, or Go's with `MaxDecoderHeaderTableSize` raised) gets no
  size update (the table's still the 4096 its decoder assumes); one lowering it below 4096 gets the smallest since the
  last block, then the final size, at the start of the next block (part 6's mechanics); and a lowering then raising
  past the cap between two blocks gets the lowered size, then 4096. A cap of 0 indexes nothing (the first block then
  starts with a size update of 0): the switch to turn indexing off, should it be needed, rather than an option for
  `none`. `session_config` stays 120 bytes (libc++; the cap takes what was padding after `connection_window`).
- `indexing::none` stays in `hpack.hpp`, but no session uses it: the encoder's default constructor still makes one, as
  HPACK's tests check its representations byte for byte (stage 1's encoder tests), and `fuzz_hpack` re-encodes with both
  modes (with `none` the table must stay empty). Its rule that the table only shrinks, which kept the sessions' bytes
  the same in part 6 when a peer offered more than 4096, is gone: the cap does that now, so both modes follow the size
  they're given the same way (two tests' expected bytes changed: `none` growing to 8192 now signals it, `3fe13f82`
  rather than `82`, and shrinking to 0 then growing back signals both, `203fe11f82` rather than `2082`; and "the encoder
  signals a smaller table, once" became "the encoder signals the table's size, once, without indexing too", 8192, 100
  then 200 giving `3f453fa90182`, 100 then 200, rather than `3f4582`).
- No existing HTTP/2 test compared a session's header blocks byte for byte, so none of their expected bytes changed:
  `test/http/http2.cpp`'s byte-level tests encode frames (and SETTINGS, PING, GOAWAY) without HPACK, and the blocks
  they send the session come from a `hpack::encoder` of their own (`none`), which the session's decoder takes as it
  did. Every HTTP/2 test (and the suite) passed unchanged with the sessions indexing.
- Tests. `test::h2_client` keeps the header blocks it receives as they were encoded (`blocks`, per stream: HEADERS'
  payload without padding or priority, with its CONTINUATIONs), and which fields came as never-indexed literals
  (`response::never_indexed`), and `test::size_updates()` reads the size updates a block starts with. Over a socket
  (cleartext, prior knowledge), the server's (`[http2][server][hpack]`): three requests whose responses repeat
  `content-type`, `x-served-by` and two fields echoing the request's (`x-token`, and `cookie`, which the h2_client's
  session, indexing now too, sends never indexed and the server decodes), each block after the first more than 20
  bytes smaller, `set-cookie` a never-indexed literal every time, and no size updates; a client offering 65536, then
  `x-a` (3000 bytes, under 3/4 of 4096), `x-b`, `x-a` again: no size updates, and the third block over 1000 bytes (`x-b`
  pushed `x-a` out, so it's a literal again: in a table of 65536 it would have been an index); and SETTINGS of 100
  then 8192 written by hand between two requests (the session can't change its own; a PING makes sure they've
  arrived), the next block starting with `3f453fe11f` (100, then 4096), the one after with none and smaller again (the
  table cut to 100 kept only its newest field). The client's (`[http2][client][hpack]`, against `test::h2_client` as the
  server): the same three, its requests repeating `:authority`, `user-agent` and `accept` (each block after the first
  more than 20 bytes smaller, the second and third the same bytes), `authorization` and `cookie` never-indexed
  literals every time, `:path` a literal (without indexing). And the session's bytes (`[session][hpack]`, with
  `raw`'s SETTINGS): a response of `:status`, `content-type` and `server` twice, whose second block is `88bfbe` (the
  two fields as dynamic indexes 63 and 62) unless the table is 0, for the peer's initial size, larger (65536: no
  update), smaller (2048: `3fe10f`), lowered then raised past the cap (`3f453fe11f`), raised then lowered back
  (none), 0 (`20`, and nothing indexed), a cap of 0 (`20`), and a cap of 8192 with the peer's 65536 (`3fe13f`) or its
  initial 4096 (none). Each was checked to fail without the cap where it should (the session's encoder given the
  peer's setting as it was).
- `fuzz_http2` gained a property and an op: after a script, the session's output is read as its peer would read it,
  with a decoder whose limit is the HEADER_TABLE_SIZE of each SETTINGS the session acknowledged, from the
  acknowledgement on (the session's `on_settings()` gives it, in the order of its ACKs), and each header block must
  decode to the fields the application submitted, in order; and op 7 (which received bytes, as 0 does) now receives a
  SETTINGS frame changing HEADER_TABLE_SIZE (to its byte's 64s, 255 being 65536), as random bytes rarely make one. With
  the session's encoder ignoring SETTINGS (a deliberate break), it failed within two minutes; with the code as it is,
  700 s (two runs, the second with `-len_control=0` for longer scripts) found nothing, nor did `fuzz_hpack` in 300 s.
- Measured (`bench_request`, Release, five alternating runs of master's binary and the branch's, both with the bytes
  count, which this part adds: a blocking relay between a second client and the server, counting each way, in a run
  of its own after the timed one). Bytes over HTTP/2, a request (`:method`, `:scheme`, `:authority`, `:path`) and its
  response (`:status`, `server`, `content-type`, `content-length`, `date`; a 13-byte body): from the second request on,
  33 bytes up and 89 down before, 21 up and 49 down after (every run the same): the request's block 24 bytes to 12
  (`:authority`, 12 bytes as a literal, now one), the response's 49 to 9 (`server`, `content-type` and `date` now one
  byte each; `content-length` stays a 5-byte literal, and `date` is a literal again once a second). The first request,
  with the connection's preface and SETTINGS, 106 up and 138 down before, 106 and 135 after: a literal with
  incremental indexing has a 6-bit prefix for its name's index where one without has 4, so `server` (54),
  `content-type` (31) and `date` (33) take a byte less each even the first time. Allocations a request (client and
  server together) 51.8 to 51.9 before and after: the decoders now insert the peer's fields, into part 6's storage,
  which allocated only while the tables first grew (in the warm-up). Time 80.6 to 84.6 us a request before, 80.0 to
  81.8 after (within the spread; loopback's bytes cost little). The rest of `bench_request` unchanged (pipelined 1139
  to 1178 ns before, 1128 to 1144 after; over a socket 27.7 to 29.1 us before, 27.7 to 29.0 after).
- Noticed while counting: a response with a Content-Length ends with an empty DATA frame (END_STREAM), 9 bytes, after
  the body's last DATA, as `finish()` ends the stream after the handler; ending it with the write that reaches the
  length would save them. Not done here (it's the response writer's, not HPACK's).
- Interop, by hand (cleartext, prior knowledge, through a throwaway Python relay printing each HEADERS block's size and
  first bytes; h2load and nghttp aren't installed, so not those). curl 8.7.1 (nghttp2 1.64.0, macOS's) with five URLs
  in one invocation against `example/http-server` (with `Authorization`, `Cookie` and a repeated custom field): one
  connection ("Re-using existing connection" each time), every response right (`hello world`, `hello, alice`,
  `hello, bob`, and a 404), the server's blocks 39 bytes then 8 (`88bfbe` and `content-length`) and 6 for the 404;
  curl's own blocks, which index, decoded by the server. Go 1.27.1's net/http (h2c through `Protocols`'
  `SetUnencryptedHTTP2`, `HTTP2Config.MaxDecoderHeaderTableSize` 4096, 65536 and 512) as the client of
  `example/http-server`, four requests each: one connection, every response right, the server's blocks 39 bytes then
  8, and with 512 the first block starting with `3fe103` (512); and as the server for a throwaway cppnet client making
  six requests on one connection (`User-Agent`, `Accept`, `Authorization`, `Cookie`, `X-N`): Go saw every field right
  on each (its handler echoing them), the client's blocks 74 bytes then 39, and with Go's 512 the second block (the
  first after Go's SETTINGS) starting with `3fe103`; with 65536, no size update.


### Part 8: Structured Fields and `http::priority` (done 2026-10-21)

- `http/structured_fields.hpp` (internal, as `hpack.hpp` is; `net::http::sf`, as RFC 9651's grammar names its rules):
  RFC 9651's parsing (section 4.2), every bare item type, inner lists and parameters, of Items and Lists as well as
  Dictionaries: they share everything but the keys (a Dictionary's members are a List's with a key in front), so the
  two more entry points cost a few lines, and with them the whole httpwg suite applies (below). `parse_item(value)`
  returns a `std::expected<sf::item, std::error_code>`; `parse_list(value, on_member(const member&))` and
  `parse_dictionary(value, on_member(std::string_view key, const member&))` call back as each member is parsed (a
  template over a function pointer and a context, as `hpack::decoder::decode()`), failing with `invalid_argument`
  for the whole value. Calling back as it goes, rather than checking the whole value first and then reporting (two
  passes), means a caller sees the members before what's malformed, and must drop what it made of them, as
  `parse_priority()` does (it gathers into locals, and applies them only if the value parsed); documented on both.
  A Dictionary's repeated keys are each reported: RFC 9651's ordered map replacing the earlier with the later (in the
  earlier's place) is the caller's, which `parse_priority()` gets by taking the last.
- Nothing's allocated. A `member` is a `std::variant<item, inner_list>`; an `item` a `bare_item` and its `parameters`;
  a `bare_item` its type, a number and a view of its text. The number is an Integer's value, a Decimal's in
  thousandths (exact, as a Decimal has at most 12 integer digits and 3 fractional ones, rather than a `double`), a
  Boolean's (0 or 1) or a Date's; `integer()`, `decimal()` (a `double`), `boolean()` and `date()` give it if the type's
  right. The text is a String's, Token's, Byte Sequence's or Display String's as the field has it, without its
  delimiters: escapes, base64 and percent-encoding still in it, so a Token's is its value, as is a String's without a
  backslash, and `sf::decode(item, out)` appends any of their values (a Display String's as UTF-8, a Byte Sequence's
  bytes). `parameters` and `inner_list` are views of their text, which the parser checked as it went, and which
  iterating reads again (`detail::view_iterator`, with the same functions the parser used; the end is
  `std::default_sentinel_t`); `parameters::find(key)` gives the last with the key. Sizes (libc++): `bare_item` 32
  bytes, `parameters` 16, `item` 48, `inner_list` 32, `member` 56.
- To RFC 9651's letter: only SP at the value's ends (a tab there fails, as the suite's "leading space" test says),
  OWS (SP or HTAB) around commas, SP within inner lists and after a `;`; keys `lcalpha / "*"` then `lcalpha / DIGIT /
  "_" / "-" / "." / "*"`; Integers at most 15 digits, Decimals 12 and 1 to 3 (`-0` is 0); Strings and Display
  Strings `%x20-7E` only (Display Strings' hex lowercase, and their bytes strict UTF-8: no overlong forms, surrogates,
  or past U+10FFFF; a lead byte followed by a plain character fails); Dates Integers, over the whole syntactic range
  (the suite's ±999,999,999,999,999 are SHOULDs we take). Byte Sequences take what the RFC says parsers SHOULD NOT
  fail on: no padding, more than needed, and bits set past the last byte (dropped by `decode()`); but `=` only at
  the end (the suite's MUST fails), and not a length of 4n + 1 characters, which no bytes can have (Go's base64
  refuses it too). Every SHOULD-level case of the suite (`can_fail`, 8 of them) is accepted, to its canonical form.
- Several lines: RFC 9651, section 4.2, has a field's lines joined with commas before parsing, which
  `parse_priority()` documents its value as. `sf::join_lines(std::span<const std::string> lines, std::string& buffer)`
  joins them with ", " (empty lines too: the suite's List of "1", "", "42" must fail, as an empty member), returning a
  single line as it is (no copy: what nearly every request has) and none as "". For part 9, the server's streams:
  `if (const auto lines = req.headers.get_all("priority")) p = parse_priority(sf::join_lines({lines->begin(),
  lines->end()}, buffer), p);` (a `values_range`'s iterators are pointers, so the span needs no copy; `buffer` a
  `std::string` of the connection's, used only when there are several lines).
- `http/priority.hpp` (public): `priority`, `parse_priority(value, base = {}) noexcept` and `to_string()` as proposed.
  Duplicates: the plan's "the last `u`, as a Dictionary does", so a Dictionary's value is its last member, and then RFC
  9218 judges it: `u=1, u=9` leaves base's urgency (9 is out of range), and `u=9, u=1` gives 1. Go's
  `parseRFC9218Priority` (from memory of x/net's, unverified) differs: its callback over `httpsfv.ParseDictionary`
  sets the urgency only for a valid member, so there the last valid one wins (`u=1, u=9` gives 1); nghttp2 (from
  memory too) ignores the whole field for an invalid `u` or `i`. Ours is what the two RFCs say read together, and
  the difference only shows for a sender repeating a key with an invalid value. Parameters on `u` or `i` are ignored
  (they're still an Integer or a Boolean), and so is an inner list. `to_string()` writes an urgency past 7 as it is
  (documented: whoever reads it ignores it), rather than clamping it.
- Tests. The httpwg's structured-field-tests (https://github.com/httpwg/structured-field-tests, commit 3c6d776; the
  IETF Trust's BSD-3-Clause-style licence, which allows redistribution with its notice, kept in the header):
  `tools/structured_fields/gen_tests.py` makes `test/http/structured_field_tests.hpp` from a checkout, 1587 cases
  from every parsing file but `large-generated.json` (its values, up to 22 KB each, are made by
  `test/http/structured_fields.cpp` instead, its 11 cases), not `serialisation-tests/` (for serializers). Each case is
  its lines, its kind (Item, List or Dictionary), whether it must or may fail, and its canonical form if that isn't its
  lines joined; a case a line, positional, between `clang-format off` and a `NOLINT` of the designated-initializer
  and raw-string checks (designated and formatted it was 395 KB; this way 180 KB, NULs and controls as octal escapes,
  string_view literals as some have NULs). The test joins the lines (`join_lines()`), parses them as their kind,
  serializes what it gets with `test/http/sf_serializer.hpp` (RFC 9651's section 4.1, test-only: the library only
  parses; shared with the fuzzer) and compares it with the canonical form: all 1587 pass, the 8 `can_fail` ones
  too. Then (`[http][structured-fields]`): a Dictionary of every type, its views within the value, decoded values,
  inner lists' items and parameters, repeated parameters and `find()`; repeated keys each reported; a failure after
  members (seen, then the error); empty Lists, Dictionaries and inner lists; numbers at their limits (Decimals in
  thousandths); `join_lines()` (no copy for one line, empty lines kept, a String split over two lines); and Display
  Strings' UTF-8 (surrogates, overlong, past U+10FFFF, cut short) and Byte Sequences' lengths and padding.
  `[http][priority]`: RFC 9218's examples, base kept for what's missing, section 8's merging (`parse_priority(response,
  parse_priority(request))`), out-of-range and wrongly typed values (every other type, an inner list, a bare `u`),
  repeated keys (last, then judged), unknown members and parameters, malformed values leaving base whole (an empty
  member, a trailing comma, an uppercase key, spaces around `=`, a 16-digit Integer, an unterminated String, a leading
  tab, a Display String that isn't UTF-8), spaces and tabs between members, lines joined, and `to_string()` for all 16
  priorities through `parse_priority()` and back.
- `fuzz_structured_fields` (its sources `structured_fields.cpp` and `priority.cpp`): the input parsed as an Item, a
  List and a Dictionary, each the same twice, and what parses serialized (canonical) parsing to the same and
  serializing the same again; `parse_priority()` over two bases ({0, false} and {7, true}) agreeing with the
  Dictionary (each base whole for a value that doesn't parse); its results through `to_string()` and back; and the
  priority checks again on the input with "u=1, i, " before it. That last was added after a deliberate break (an
  invalid `u` not replacing a valid one, i.e. Go's rule) went unfound for 300 s three times without it (random bytes
  rarely make a valid `u` then an invalid one); with it, the break was found within 2 s in each of three runs. Seeded
  with the suite's values, 300 s (1.1 million runs) and 600 s (1.9 million) found nothing on the code before that
  check, and 600 s (1.4 million) on the final code; the 10 s ctest run passes.
- What part 9 needs from this: `parse_priority()` over the default for the request's lines (and over the stream's
  for the response's, section 8's merging; over the default again for a PRIORITY_UPDATE, which carries a complete
  set), and `join_lines()` for the lines; nothing else.

### Part 9: priority signals (done 2026-10-21)

- The session (`http2.hpp`, `http2_session.hpp`): `frame_type::priority_update` (0x10) and
  `settings::no_rfc7540_priorities` (`setting_id` 0x9 was there; `encode_settings()` writes it when it differs, after
  MAX_HEADER_LIST_SIZE, by id). Both sides' sessions send it as 1 in their first SETTINGS, set in the constructor as
  ENABLE_PUSH's 0 is, so the server's and the client's connections send it without a change of their own (and
  `server_config::http2_priorities = false` still sends it: what it says is that RFC 7540's signals are ignored, which
  they are either way, as Go's server sends it with either scheduler). `settings` stays 24 bytes (libc++), the bool in
  the padding after `enable_push`. Received: a value over 1 is PROTOCOL_ERROR, and so is a later SETTINGS whose value
  differs from what the first said (left out of the first is its initial 0: so a first without it and a later 1 is a
  change, a later 0 isn't; a later SETTINGS without it changes nothing). Within the first frame, the last value wins, as
  for any setting. `apply_setting()` takes whether it's the first frame's. RFC 9218 only says a change MAY be an error;
  Go checks only the value (and only its server), so this is stricter than Go, as the plan said. Both roles are checked
  alike.
- PRIORITY_UPDATE (`session::on_priority_update()`), in the order the RFC's errors come: a client's session takes any
  as PROTOCOL_ERROR; a server's, one on a stream other than 0 is PROTOCOL_ERROR, one under 4 bytes FRAME_SIZE_ERROR, a
  prioritized id of 0, or an even one (a push stream: this side never pushes, so it's idle for good), PROTOCOL_ERROR.
  Frames past MAX_FRAME_SIZE and one inside a header block are connection errors before it's looked at, as any frame's.
  Then by the stream's state: after this side's GOAWAY, one for a stream past its last that isn't open is ignored
  without counting (as the frames of streams the peer may not know are refused, Learned in stage 2); a closed stream's
  is ignored and counted as a glitch (Decided question 22), whatever its value; then the value, which must parse as a
  Dictionary (`sf::parse_dictionary()` with a callback that does nothing, then `parse_priority()`: parsed twice, as
  `parse_priority()` doesn't say whether it parsed, only for this frame, rather than a new internal function), else
  it's ignored and counted. A parsed one is `parse_priority(value)` over the default: a complete set (RFC 9218, section
  7.1), so "i" alone is u=3 incremental, and "" is u=3. For an open stream (one the application knows, as every open
  stream of a server's does) it's `session_events::on_priority_update(stream, priority)`, a new pure virtual (the
  client's connection's is a no-op, as its session never calls it; the tests' recorders log it). For an idle one it's
  kept, the latest per stream, in `idle_priorities` (a vector of stream id and priority, 8 bytes each), searched
  through: the RFC's bound, the open streams (`open_peer_streams`) plus those kept, is at most MAX_CONCURRENT_STREAMS,
  PROTOCOL_ERROR past it (a replacement doesn't count); and, should MAX_CONCURRENT_STREAMS be larger (or unlimited, as a
  `session_config` may say), at most 128 (`max_idle_priorities`), past which they're ignored as glitches, so neither the
  memory nor the search grows with a client that sends them for ever higher ids. Each kept one is past
  `highest_peer_stream`: `end_header_block()` takes the block's stream's, if any, right after `stream_for_block()`,
  whatever becomes of the block (`take_idle_priority()`, which also drops those of lower ids, closed now that a higher
  stream has opened), so a refused or malformed head drops it too; and it's reported right after the head's
  `on_headers()`, so the connection, having made the stream from the head (and its field), replaces that with the
  update: the most recent signal, as the RFC has it. The check costs a request nothing when none are kept (`empty()`
  first).
- The server (`http2_server.cpp`): each `http2_stream` has `priority` (2 bytes) and `prioritized` (1), guarded by the
  connection's lock, in what was padding (between `expects_continue` and the `body` pointer), so the stream's no larger.
  `start_stream()` notes, as it adds each regular field to the request, whether it's `priority` or an intermediary's
  (`via`, `forwarded`, `x-forwarded-for`): `priority_signal_of(name)`, a switch on the name's length, comparing only
  names of 3, 8, 9 or 15 characters, so a request without them costs a switch a field; then, before anything that may
  refuse the request (a 501, a 400, a 417, whose fields still tell of the client, as Go's `processHeaders` reads them
  before its handler could refuse), `request_priority(req, signals)`: with `http2_priorities` off, u=3 incremental; a
  request with one of the intermediary's fields makes the connection `behind_intermediary` (sticky), whose requests are
  then u=3 incremental, their fields ignored; else one with `priority` makes the connection `priority_aware` (sticky:
  the field present, even malformed, as Go's), and is `parse_priority(sf::join_lines(lines, priority_lines))` over RFC
  9218's default (the field's lines from `get_all()`, joined into the connection's `priority_lines` only when there are
  several); and one without is u=3, incremental unless the connection is aware. `on_priority_update()` (the session's
  event) replaces the stream's priority, and makes the connection aware. `http2_send_head()` merges the final
  response's `priority` field over the stream's (`parse_priority(value, stream.priority)`, section 8: the response's
  parameters replace the client's, the others kept), if the stream's prioritized, and not for 1xx (whose headers the
  final response sends again anyway); a field of several lines is joined in a local string, as that's rare. With
  `http2_priorities` off, `prioritized` is false and the session's PRIORITY_UPDATEs are still checked (its errors are
  the protocol's) but the connection ignores them, so every stream stays u=3 incremental, as the plan says.
- Departures from Go (Decided question 19 is Go's rule, and these are where following the plan's wording differs from
  Go's code, `processHeaders` and `rfc9218Priority` in `src/net/http/internal/http2`, Go 1.27.1, read for this part):
  Go parses the field of the request that first shows an intermediary (over u=3 incremental) and ignores only later
  ones; here that request's is ignored too, as "request fields ignored" says. Go takes a request's last `priority`
  line; here the lines are joined (RFC 9651's rule, part 8). Go makes the connection aware on any PRIORITY_UPDATE, even
  one it then refuses as a stream error, or one for a closed stream; here only one the session reports does, and one
  kept for an idle stream only once that stream's head arrives (so a request in between, without a field, is still
  u=3 incremental). And Go resets the stream for a PRIORITY_UPDATE that doesn't parse; here it's ignored (Decided
  question 22).
- `server_config::http2_priorities` (public, on) came with this part rather than with part 10, as what it turns off
  is recorded here: `http2_options::priorities` carries it to the connection. Nothing schedules by priorities yet, so
  there's no public way to see a stream's: `detail::stream_priority(const server_request&)` (`http/http2_server.hpp`,
  internal, for tests) finds the request's stream through its body (a `stream_reader`, whose new `owner()` is the
  stream) and reads its priority under the lock; it's nullopt over HTTP/1.1. Part 10's scheduler will read
  `http2_stream::priority` directly.
- Tests. On bytes (`test/http/http2.cpp`, `[http][http2][session][priority]`): both sides' first SETTINGS have 9=1, and
  each records the other's; NO_RFC7540_PRIORITIES of 0, 1, 2 and 2^32-1, then 1 again, left out, 0 after 1, 0 and 1
  after it was left out, 0 then 1 in the first frame, and 1 then 0 in a later one, as a server and as a client; each of
  PRIORITY_UPDATE's connection errors (on a stream, short, stream 0, an even stream, inside a header block) with its
  GOAWAY; a client receiving one; reports for open streams, complete sets ("u=5, i", "i", "u=0, x=?1;y, i=?0", "", and
  "u=9, i=1, u=6": the last u, and i=1 an Integer, ignored), a half-closed stream's too; kept ones reported after their
  heads (the latest of two, over the request's field), a stream between without one; one dropped with a malformed head,
  and one with a refused head (RST_STREAMs checked); the bound by MAX_CONCURRENT_STREAMS (two kept, a third an error;
  or a higher stream opening, dropping them, then one kept and one an error); the 128 with MAX_CONCURRENT_STREAMS
  unlimited (the 129th a glitch, the next past the budget); a closed stream's, an open one's that doesn't parse, and an
  idle one's that doesn't parse, each a glitch (one allowed, every pair of them failing the connection, and none
  reported nor kept); and one after GOAWAY ignored and not counted (no glitches allowed), an open stream's still
  reported. Over a socket (`test/http/server_priority.cpp`, `[http][http2][server][priority]`, `test::h2_client`, which
  gained `priority_update(id, value)`: the session's output, then the frame, written at once), a handler reporting its
  stream's priority as it starts, after waiting at a gate (for an update sent meanwhile), and once its head is sent:
  the defaults before and after the client's first field, a field's parameters over the default, two lines joined, a
  malformed field ignored but still a signal; each intermediary field ignoring that request's field and the
  connection's later ones, but not its PRIORITY_UPDATE nor the response's field; an update before the head as the first
  signal (then a request without a field is u=3), an update before a head that has a field (the update wins), one while
  the handler waits, and one for a stream that's ended (ignored, the connection going on); the response's field merged
  (u replaced and i kept, i replaced and u kept, invalid members ignored, a malformed field ignored, over the
  unprioritized default) and sent to the client; `http2_priorities = false` (field, response field and update all
  ignored, the connection going on); and an HTTP/1.1 request, with no stream priority. Each server rule was checked to
  fail a test when broken (the update not making the connection aware failed the update test once its first request had
  no field: with one, the field made it aware anyway, so the test was changed to start without one; and kept updates
  not dropped as higher streams open failed the bound test).
- `fuzz_http2`: op 7's bits 3-4 choose among four frames random bytes rarely make: the SETTINGS changing
  HEADER_TABLE_SIZE (part 7's, now 0), SETTINGS with NO_RFC7540_PRIORITIES of the next byte, a PRIORITY_UPDATE (for a
  stream the application knows, an idle one, an even one or 0; with "u=1", "u=6, i", "u=" or bytes from the script), and
  a malformed one (on a stream, or short); its recorder logs `on_priority_update`, and the property (the same at once or
  a byte at a time, and the blocks decoding) is unchanged. The scripts' prefix SETTINGS now have NO_RFC7540_PRIORITIES =
  1, as a real peer's do (with an empty one, any 1 later would be a change). Seeded by hand with each kind (the events
  of each seed checked once, by a temporary print: a kept update reported after its head, an open stream's, the setting
  and the errors), 360 s (118,000 runs) and then 600 s with `-len_control=0` (113,000 runs, scripts up to 512 bytes)
  found nothing; the 10 s ctest run passes. (No deliberate break was tried for this part: the property is the same as
  before, and what's new reaches it through the events the recorder logs.)
- Measured (`bench_request`, Release, five alternating runs of master's binary and the branch's; its HTTP/2 requests
  have no `priority` field, as most clients' don't). Over HTTP/2, a stream at a time: 79.3 to 82.2 us a request before,
  79.8 to 86.2 after (medians 81.3 and 82.1: within the spread, which one run's 86.2 widens), and 51.8 to 52.0
  allocations a request both (nothing allocates for a request without the field: the signals are noted as its fields
  are added, and the response's `get_all("priority")` is a look through its few fields). Bytes: the first request's 106
  up and 135 down became 112 and 141, the SETTINGS_NO_RFC7540_PRIORITIES each side now sends (6 bytes), and from the
  second on 21 and 49, unchanged. HTTP/1.1, untouched: pipelined 1137 to 1153 ns before, 1145 to 1173 after (part 7
  saw 1128 to 1178 on one binary), over a socket 27.6 to 29.8 us before, 27.7 to 29.1 after.
- For part 10: the scheduler reads `http2_stream::priority` (under the lock, as everything it touches); a change of it
  (an update, or the response's field, which `http2_send_head()` merges before the head's submitted) is where a stream
  would move between levels (`update(stream, priority)` in the plan). The default for clients that never signal is
  incremental, so with the scheduler they share the connection round-robin, as now.

### Part 10: scheduling by priority (done 2026-10-21)

- The scheduler (`http/http2_scheduler.hpp`, internal, without I/O): `scheduler_node` is a stream's place, its id, its
  priority (the stream's, which part 9 kept in `http2_stream`: now `current_priority()`, set through `update()`) and
  intrusive links; `http2_stream_base` derives from it, so a stream joining allocates nothing (24 bytes a stream, the id
  repeated). `http2_scheduler` has eight levels (an urgency past 7, which `parse_priority()` never makes, is 7's), each
  a circular list of its non-incremental streams by id (a stream joins from the end, as it usually has the highest id)
  and a ring of its incremental ones (joining at the end: just before the one that goes next). `next(ready)` takes the
  most urgent level with a ready stream; within it, the kind whose turn it is goes if it has one ready, and passes the
  turn, otherwise the other goes and the turn stays (so a kind that had none ready goes first once it has); turns start
  with incremental, as Go's do. Go flips one toggle for the connection at every frame, whatever went; this flips a
  level's only when its turn's kind went, which is the same when both kinds are ready, and exact otherwise. The first
  ready non-incremental stream by id goes (Go's moves its list's head to the one that went, so after a blocked stream
  its order is no longer by id); the ring moves on past the one that went. `update()` to the same priority doesn't move
  a stream (a client repeating a PRIORITY_UPDATE doesn't send its stream to the end of its ring). Only streams with a
  write waiting are in it, so readiness is a window: the connection's (`session::send_window(0)`, new, checked once
  before looking through) and the stream's.
- Writing (`http2_connection_base`; the server's only, `scheduling`, from the session's role: the client's request
  bodies go as before). `stream_writer::write()` is no longer a coroutine: `step_write()` takes a step under the lock,
  and a write that doesn't have to wait returns a ready task (as the plan wanted the uncontended path to cost nothing;
  it costs less: a write's coroutine frame, an allocation, and the second lock `wake_writer()` took); one that has to,
  `write_waiting()`, a coroutine. Uncontended (Decided question 21): while no stream's queued and less than `max_ahead`
  (64 KiB) of DATA waits for the writer or is being written (`data_queued`, `data_writing`, which the writer sets from
  the first as it takes the output, and clears once it's written: so what's in the writer's hands counts too, else
  another 64 KiB could go direct while it writes), and the output's under `max_output`, a write submits a frame at a
  time (each cut as the session cuts them: so a response's frames are the same whichever way it goes). What doesn't go
  (past `max_ahead`, or without a window) joins the scheduler as its span (`sending`, `sending_done`), and waits on the
  stream's notifier: a write waiting for a window is queued too, rather than waiting for its own wakeup, so the writer
  frames it when the window grows, and the handler isn't woken for nothing. The writer, each time round, under the lock:
  `fill()`, then `take_output()` (control frames and heads first, as they were submitted, then the DATA `fill()`
  framed). `fill()` takes `next()` a frame at a time (at most MAX_FRAME_SIZE), until `max_ahead`, waking each stream
  whose span is all framed (taken out first). A stream that fails is taken out by `fail_stream()` (and
  `remove_stream()`), under the lock, so the writer never touches the span of a write that's returned. As a write
  returns only once it's all framed, a stream's end (an empty DATA, or trailers) follows its data.
- So a single write larger than `max_ahead` (a response alone) now goes 64 KiB a round through the scheduler, and
  returns once it's framed (64 KiB, and TCP's buffers, from the end), where it was copied into the session whole (as far
  as the windows allowed) and returned at once: the same frames (a test checks a 1 MiB write's are 64 whole frames), in
  more, smaller writes to the socket, and a handler now feels the socket (`test/http/http2_server.cpp`'s test of a
  connection ending with much still to send relied on the handler returning with 8 MiB queued: its client now reads
  until the handler's returned, about 1 MiB, here, still to come, then stops).
- Holding a turn (not in the plan; learned by hand, through a throttling relay, below): a handler that writes a frame at
  a time (16 KiB, as copying a file does) leaves the scheduler each time its write's framed, and is back microseconds
  later; but the writer, if the socket takes what it framed at once, is back sooner, and filled the round with the
  others. Ending the round there wasn't enough (a u=0 response, written 16 KiB at a time, got half the connection from
  u=7 ones). So when a write's all framed and its stream would lose its turn (it's non-incremental, or its urgency has
  no one left, and others are queued), the round ends and its turn's held (`turn_held`): nothing more is framed until
  it's queued again, has ended or failed, or `hold_turn` (1 ms) has passed (the timer wakes the writer for it), so a
  handler that's slow to write again (e.g. reading what it sends from elsewhere) holds the connection up for a
  millisecond at most each time. Then the u=0 response had all of it. Incremental streams sharing an urgency don't hold
  (they'd share anyway). Go has the same gap (its handlers wait for each 4 KiB chunk to be written) without the hold.
- Deadlines, with a write waiting in the scheduler: queuing arms the write deadline (as a wait for a window did);
  framing some of it resets it and arms it again (it's still waiting), a window growing likewise (but the waiting write
  isn't woken); the timer resets a stream whose deadline passes, which `fail_stream()` takes out of the scheduler, and
  the write returns `timed_out` with what was framed. But a write waiting its turn (it has a window, and the writer has
  written within its timeout: `waiting_turn()`, from `wrote_at`, the time of the writer's last write while streams were
  queued) has its deadline put off, from when the writer last wrote, rather than being reset: a bulk response behind an
  urgent one that takes the connection for longer than `write_timeout` isn't the client's fault (a test reads slowly so
  an urgent 6 MiB takes two seconds while a u=7 one waits, with a one-second `write_timeout`; it failed with the check
  off). A client's reset, the connection ending (its streams fail) and a graceful close (GOAWAY; queued writes go on)
  work as they did, as tests check.
- `http2_priorities = false`: every stream's u=3 incremental (part 9), so contended responses take turns, a frame each.
- Departures from the plan: a write without a window queues (rather than waiting for its window itself); the writer's
  output counts against `max_ahead` while it's written; the hold; deadlines put off while waiting a turn; and the writes
  that don't wait return without a coroutine. The plan's TCP_NOTSENT_LOWAT stays later, and matters: macOS's autotuned
  send buffer (up to 4 MiB) is past reordering, so by hand a u=0 response started a second into the others went after 2
  MiB of theirs (below), until the server's send buffer was set small.
- Tests. The scheduler alone (`test/http/http2_scheduler.cpp`, `[http][http2][scheduler]`): urgency order, the
  non-incremental by id (and one queued later with a lower id going first), the ring (a stream joining last), the turns,
  streams not ready (each kind; a kind keeping its turn; a less urgent level going meanwhile; none), updates (up a
  level, to the end of a ring, into a level's ordered list, the same not moving, one not queued only taking it),
  removals (the ring's next, the ordered list's first, last and only, one not queued) and an urgency past 7. Over a
  socket (`test/http/server_scheduling.cpp`, `[http][http2][server][scheduler]`), with `test::h2_client` (which gained
  `hold_windows`, not consuming DATA, and `data_log`, the non-empty DATA frames in order): the client uses up the
  connection's 65535 bytes with a first response (the plug) it never gives back, requests the responses under test (each
  written in one write), which all queue, and a probe request whose handler waits until they have
  (`detail::wait_queued_streams(req, n)`, internal, which waits on the writer's notifier: a write that queues wakes the
  writer); then consuming the plug's data (the client's session sends WINDOW_UPDATE at once, as its window's empty) lets
  the server send, in the scheduler's order. So: urgent first ("UUNNBBB"), non-incremental by id whatever order their
  handlers queued in, incremental in rounds (in the order they queued, so the test checks the rounds), the two kinds
  taking turns, clients that never signal and `http2_priorities = false` in rounds, a PRIORITY_UPDATE after the first
  frame, a response's Priority lowering its stream, trailers after the data (a DATA after them would be malformed), a
  queued write timing out (and its handler's token stopped, waited for, as it's stopped once the lock's released), reset
  by the client (its write returning what was framed, `cancel`, the rest going to the other), the client closing the
  connection, the server's `close()` (the queued responses finishing, then the connection ending), a response alone
  (whole frames), and the waiting turn above. 50 runs of `[http2][scheduler]` in a row, and the suite with `ctest -j8`,
  passed.
- `fuzz_http2_scheduler` (new: cheap, and the intrusive lists are where a bug would hide): a script of adds, removes,
  updates and `next()`s (with a mask of streams not ready, and of those to take out as they're picked) over 16 streams,
  through the scheduler and a model of plain containers (a sorted vector and a deque per level), which must pick the
  same, and agree on what's queued. 300 s (806,000 runs) found nothing; two deliberate breaks (not moving the ring's
  first when it's removed; inserting into the ordered list one place off) were found within seconds.
- Measured (`bench_request`, Release, five alternating runs of master's binary and the branch's, the new case built into
  both). Over HTTP/2, a stream at a time: 80.6 to 86.7 us a request before (median 81.9), 82.0 to 86.9 after (median
  83.8: within the spread, as part 9's 79.3 to 86.2 was), and 51.8 allocations a request before, 49.6 after (the body's
  write, which doesn't wait, without its coroutine frame). The new case, 8 streams at once over one connection from
  cppnet's client, 256 KiB each written 64 KiB at a time (u=3 incremental: the client sends no priorities): 1250 to 1315
  us a round before (median 1288, about 1550 MiB/s), 1215 to 1322 after (median 1219, about 1640 MiB/s), and 90.9
  allocations a response before, 96.3 after (the writes that queue, each with its coroutine, and the writer's smaller
  rounds; with the windows of 64 KiB cppnet's client gives a stream, most writes wait either way). HTTP/1.1, untouched:
  pipelined 1168 to 1187 ns before, 1175 to 1191 after; over a socket 27.8 to 29.9 us before, 28.0 to 32.2 after.
- Interop by hand (throwaway programs in the scratchpad): a server answering `/bytes/:n` (written 16 KiB at a time).
  curl 8.7.1 (nghttp2 1.64), `--http2-prior-knowledge --parallel`, multiplexing on one connection: 6 responses of 10
  bytes to 3 MB, and 3 rounds of 20 of 8 MiB, every body compared and right. Go 1.27.1's client (`Protocols`'
  `SetUnencryptedHTTP2`), 7 responses at once with Priority fields (two u=7 of 2 MiB, two u=3, two u=5 incremental, and
  a u=0 started a second later, 512 KiB each), through a Python relay passing 2 MB/s to the client: every body right;
  without the hold, the u=0 one took about 600 ms to frame (half the connection); with it, about 240 ms (all of it: 512
  KiB at 2 MB/s), the u=3 ones one after the other, the u=5 ones together, then the u=7 ones one after the other. With
  the server's send buffer left to autotune, the u=0 response went after about 2 MiB already in it (above). Directly (no
  relay), everything's done in 20 ms, before the u=0 one starts. No h2load nor nghttp (not installed).

## Learned in stage 6

### Part 1: ending streams with their last DATA (done 2026-10-21)

- As proposed (see "Ending a stream with its last DATA"), on both sides. `http2_stream_base::data_left` (an
  `std::optional<std::size_t>`, 16 bytes in the base) is what's left to send of a body whose last DATA frame ends the
  stream. Every write path submits DATA through one function, `http2_connection_base::submit_body()` (holding the
  lock): it asks the session for END_STREAM when the data is exactly what's left, and marks the stream `ended` once
  it's all gone (the session sets END_STREAM only on the frame that sends the last of the data, so a write cut short by
  the windows doesn't end it). `submit_at_once()`, `fill()` and `submit_windowed()` call it; nothing else changed in
  them. A handler can't write past what's left, as `response_writer`'s `limit_writer` (and the client's
  `limit_reader`) cut a write to it, so `data_left` never goes below 0 (it's clamped regardless).
- The server: `http2_stream_ops::send_head` takes the body's length, an `std::optional<std::size_t>`, in place of its
  `bool end_stream` (from which the old flag follows: 0 ends the stream with the head): `response_writer::start()`
  passes the Content-Length (0 for a status without content), and none for `send(status)` and interim heads (and
  `send_continue()`'s 100). `http2_send_head()` sets `data_left` when the head is final, doesn't end the stream, and no
  trailers nor `Trailer` field are set (Decided question 26 (a)); HEAD's head ends the stream as before.
  `http2_finish()` then finds the stream ended and sends nothing, logging at debug if trailers were set since
  (through `http2_connection_base::get_logger()`, new: the free function has no logger of its own).
  `response_writer::trailers()`' comment, and the guide, say that such a response's trailers must be set or declared
  by the time its head is sent, and show middleware declaring them before `next()`.
- The client: `open()` takes `ends_after` (an `std::optional<std::size_t>`) in place of its `bool end_stream`: 0 ends
  the stream with the head (no body, no `make_trailers`), more sets `data_left` (a Content-Length and no
  `make_trailers`), none leaves the end to `end_request()` (trailers, or a body of unknown length), which returns at
  once for a stream that's ended.
- Not in the plan: `fill()` doesn't hold the turn of a stream its frame has just ended (part 10's hold, for a handler's
  next write, which won't come); `turn_still_held()` would have let it go at the next round anyway, but the round would
  have ended there. And a handler that throws (or is stopped) after writing all of its Content-Length now leaves a
  complete response, as one without a body already did: the stream ended with its last DATA, and the reset
  `run_stream()` then asks for does nothing if the request has ended too (the session has forgotten the stream), or is
  an RST_STREAM after the response's END_STREAM if the request's body is still coming. That's Decided question 26 (a)'s
  point (the response is complete as soon as its last byte is), so it's noted rather than changed; one written short
  is reset as before.
- Tests (no network). `test::h2_client` records the length of the DATA frame that ended a stream, if one did
  (`response::ended_by_data`). `test/http/server_end_stream.cpp` (`[http][http2][server][end-stream]`): a response with
  a Content-Length ends with its last DATA, written in one write, in several, cut into frames, in whole frames, and
  writes of a frame and more (on connections of their own, so the writes go at once), and a handler that throws once
  it's written it all (complete, not reset); one larger than its stream's window, whose rest the writer frames as the
  window grows; one without a body and one to HEAD end with their heads, and ones sent with `send(status)` (5 bytes, and
  none) with an empty DATA; a handler writing less than its Content-Length is reset (INTERNAL_ERROR, no frame ended the
  stream). `test/http/server_scheduling.cpp`: three Content-Length responses contending, in rounds, each ending with its
  last frame (one of a frame and 100 bytes). `test/http/http2_client.cpp` (`[http][http2][client][end-stream]`, against
  `test::h2_client` as the server): a request body with a Content-Length (5 bytes, and 40000 in three frames) ends with
  its last DATA, one with `make_trailers` with them (its DATA not ending it), one of unknown length with an empty DATA,
  one of 0 and its head, and `make_trailers` without a body after the head. Changed: `server_trailers.cpp`'s "whatever
  its body" test now has a Content-Length response's trailers declared (`Trailer`) before its head, or set before it,
  and checks that ones set after, undeclared, are dropped (the last DATA ended the stream); its middleware
  (`served_by`) declares its trailer before `next()`, as the guide now says to; `client_trailers.cpp`'s Content-Length
  route declares its trailers, and a route that doesn't checks that HTTP/2 drops them too; and
  `server_scheduling.cpp`'s response alone now has 64 DATA frames for its 1 MiB, not 65 (no empty one after them).
  With `submit_body()` never asking for END_STREAM, the new tests fail (both sides', and the contended one).
- `fuzz_http2` is unchanged, as the plan expected: the session's interface didn't change (its scripts already end
  streams with DATA that carries END_STREAM). Its 10-second run, and the other fuzzers', pass.
- Measured (`bench_request`, Release, eleven alternating runs of master's binary and the branch's, on a machine busy
  with other work: a load average near 6, so the times spread widely). Bytes: the first request's response 141 bytes
  down before, 132 after, and every later one 49 before, 40 after (21 up, unchanged: the request has no body): the 9
  bytes of the empty DATA frame, as the plan expected. (One run's second response took 63 bytes: its `date` changed
  second, so it went as a new literal, not an index.) Over HTTP/2, a stream at a time: 85.4 to 141.9 us a request before
  (median 106.0), 84.9 to 190.8 after (median 107.1), and 49.3 to 49.6 allocations a request before (median 49.4), 49.1
  to 49.4 after (median 49.3). 8 streams at once: 1372 to 1988 us a round before (median 1523), 1313 to 2731 after
  (median 1524), and 96.2 to 98.8 allocations a response before (median 97.3), 95.9 to 98.5 after (median 96.6). So no
  difference in time that the noise shows, and slightly fewer allocations, as a frame less goes through the writer.

### Part 2: 431 for a header list over the limit (done 2026-10-21)

- As proposed (see "431 for a header list over the limit", Decided question 27 (a)). The session's `end_header_block()`:
  a block whose decoded list is over MAX_HEADER_LIST_SIZE (`value_too_large` from the decoder, which decoded it to its
  end without calling back, so HPACK's table is in step) is, for a server's session on a stream without its head yet,
  counted as a glitch (`glitch()`, so the budget still stops a client sending nothing else), then the stream marked
  `head_received` and `known`, and reported with `session_events::on_header_list_too_large(stream, end_stream)` (new,
  pure virtual; no fields, as the decoder kept none); a block that ends the stream closes its remote side after, as a
  head's does. Anything else over the limit, trailers (`head_received`) and a client's session's response heads
  (informational ones too), is `stream_error()`'s PROTOCOL_ERROR, as before. The checks before it are unchanged: a
  closed stream and a self-dependent priority are reset first. A PRIORITY_UPDATE kept for the stream is taken
  (`take_idle_priority()` runs before) and not reported, so it's dropped, as for a malformed head.
- The server's `on_header_list_too_large()` answers with `refuse(id, status::REQUEST_HEADER_FIELDS_TOO_LARGE,
  end_stream)` (unless the connection's `ending()`, as `on_headers()` does nothing then), logged at debug: `:status`
  and `date`, END_STREAM, and RST_STREAM(NO_ERROR) if the request goes on, so the session forgets the stream and
  ignores its DATA (the reset's remembered). No `http2_stream` is made: no handler, nothing counted against
  `http2_max_concurrent_streams` nor the queue, and the stream never reaches the scheduler. The client's connection's
  override is a no-op (its session never calls it), and so are `test::h2_client`'s and `test/http/http2_connection.cpp`'s
  `test_connection`'s; `test/http/http2.cpp`'s recorder logs "too large <id>[ end]", `fuzz_http2`'s too (and counts
  the stream as one the application knows, so the script's ops may answer or reset it).
- Nothing departed from the plan. One thing the session test showed: the client's RST_STREAM for a response head over
  its limit means nothing to a server whose response and request have both ended (the session has forgotten the
  stream), as before.
- Tests (no network). `test/http/http2.cpp` (`[http][http2][session][limits]`, a list limit of 200 bytes and a field of
  238): a request's head over the limit reported (not reset) with END_STREAM, its stream then open for the answer,
  which ends it; and without, its DATA then reported as the request's body, or ignored once the application has
  answered and reset it (RST_STREAM(NO_ERROR) in the output), and a HEADERS after it taken as trailers; no GOAWAY
  either way. Trailers over the limit reset the stream (PROTOCOL_ERROR, both sides told), and a client's session resets
  a response's head over its limit, as before. Each is a glitch: with a budget of one, the second request head, the
  second trailers or the second response head over the limit fails the connection (ENHANCE_YOUR_CALM).
  `test/http/http2_server.cpp` (`[http][http2][server][limits]`, `max_header_bytes` 1024, so a list of 4,224): two
  requests whose lists are past it (a field of 2,038 three times: the first indexed by the client's HPACK table, the
  others indexes) get 431 with only `date`, ending their streams, one that ended its own and one whose body was to
  follow, which also gets RST_STREAM(NO_ERROR) (checked after a PING's answer); then a request repeating the fields as
  indexes of the table the long blocks filled is answered with them, so the tables are in step; no handler ran but
  that one, and no GOAWAY. With the session resetting such heads as before, the new tests fail.
- `fuzz_http2`'s properties are unchanged; its recorder implements the new event. Its 10-second run, and a 90-second
  one, pass.

### Part 3: TCP_NOTSENT_LOWAT, the socket's part (done 2026-10-21)

- As proposed (see "TCP_NOTSENT_LOWAT", Decided question 29 (a)), but for the result's type.
  `tcp_socket::set_notsent_lowat(bytes)` (beside `set_nodelay()`, `const`, `std::expected<void, std::error_code>`) sets
  TCP_NOTSENT_LOWAT, more than `INT_MAX` clamped to it, 0 turning it off (on Linux, back to the sysctl's, unlimited by
  default); where `<netinet/tcp.h>` has no TCP_NOTSENT_LOWAT (FreeBSD's, from memory) it fails with `not_supported`.
  `socket::wait_writable()` asks `poll()` (no wait) first and returns a ready task if the socket's writable (no
  coroutine, as a write that doesn't wait); otherwise it claims the socket's write operation in a mode of its own
  (`write_operation::claim_writable()`, `only_writable`), whose `perform()` (under the shard's lock, as a write's)
  polls rather than sends: so the scheduler's waits, deadlines, timeout and stop token are the write's, a stale `ready`
  flag is asked about again (`perform()` from `suspend_handle()`), and so is every event, until `poll()` says writable.
  A POLLERR or POLLHUP counts as writable (the write then says why); POLLNVAL is `bad_file_descriptor`. A write's own
  path gained a store in `claim()` and a branch in `perform()`, both only when it waits, so `bench_request` wasn't run.
- Departure: `wait_writable()` returns `coro::task<io::result>` (count 0, as a write of nothing), not a task of an
  `std::error_code`. It's the write operation's task (a `coro::operation<io::result>`), whose claim is what keeps it
  from overlapping a write (whichever comes second gets `device_or_resource_busy`, as a second write does); a task of
  another type would need an operation of its own, so a second claim and a second `handle_operation`'s state in every
  socket (some 200 bytes), with the overlap then refused only by the scheduler's one waiter a direction, or a claim
  shared across two operations, which `release()` (not virtual, for HALO) can't do. Part 4's writer reads `.err`.
- The kernels. macOS (Darwin 24.6), by probes over loopback: `send()` ignores the mark (a writer with a mark of 4,096
  wrote the peer's whole window, then its send buffer's worth); `poll()` says not writable once what's not sent is past
  the mark, but with Nagle's algorithm on not before it's past a segment (16,384 unsent with 4 KiB writes, against
  8,192 with TCP_NODELAY, which the server sets on its connections); 0 reads back as 0. Over macOS's loopback, which
  delivers apart from `send()`, a socket found not writable right after a burst is often writable again without the
  peer reading, as an acknowledgement still to come opens the window (in the probes, about one time in twenty with a
  mark): so "not writable" isn't a settled state until nothing's in flight. Then the peer's window is closed, or
  too small for macOS's sender to use (it held about 3 KiB of window unused for over a second, its send window
  reported by TCP_CONNECTION_INFO), and it stays so until the peer reads. Also seen: a listener's SO_RCVBUF didn't keep
  the accepted socket's receive buffer from growing (it read back 326,640 for 16,384). Linux, from memory of its sources
  (no Linux here; CI's jobs are the check): `tcp_poll()` says writable once what's not sent is under half the mark
  (`__sk_stream_is_writeable(sk, 1)`), as the EPOLLOUT wakeup does, and sets SOCK_NOSPACE when it isn't, so the wakeup
  comes; `send()` stops at the mark. So a `poll()` after an edge agrees with it on Linux, and on macOS the edge and
  `poll()` apply the same check; what the second look is for is the flag an edge left while nothing waited.
- Tests (no network; `test/socket.cpp`, `[socket][wait-writable]`, and `test/tcp.cpp`'s `[tcp][notsent-lowat]`): the
  mark read back with `getsockopt()` (16,384; 0; and `INT_MAX` for 2^40). Over a loopback connection with small buffers
  and TCP_NODELAY, whose peer doesn't read: `wait_writable()` returns a ready task on a fresh socket (with a mark and
  without), and `bad_file_descriptor` on a closed one; once filled, it waits until the peer reads (resumed by hand, it
  hasn't finished, then the peer drains until it has), with a mark and without, and with a mark it isn't writable
  though what the socket holds is under half its send buffer; it ends with `timed_out` at the write deadline and the
  write timeout, `operation_canceled` on the stop token; and a write that has to wait while it waits gets
  `device_or_resource_busy`, as does a second wait, and so does a wait while a write waits, each going on to finish when
  the peer reads. The fill writes while `poll()` says writable, until it isn't and nothing's in flight (Linux:
  SIOCOUTQ equal to SIOCOUTQNSD; macOS: TCP_CONNECTION_INFO's bytes sent, less those sent again, equal to what's been
  acknowledged, what's written less SO_NWRITE), polling as the existing socket tests' `eventually()` does: stopping at
  the first "not writable" failed about one run in three on macOS (mostly the cases without a mark); settled, 250 runs
  of the new tests passed. The
  writes during the fill leave the scheduler a `ready` flag every time: with `perform()` not asking `poll()`, the
  waiting tests fail every run (the wait ends at once); without the fast path, the ready-task test fails; with the
  option not set, the read-back and the under-half-the-buffer check fail.

### Part 4: TCP_NOTSENT_LOWAT, the server's writer (done 2026-10-21)

- As proposed (see "TCP_NOTSENT_LOWAT", Decided questions 28 (a) and 29 (a)). `server_config::http2_notsent_lowat`
  (`std::size_t`, 16 KiB; 0 sets none), passed through `detail::http2_options::notsent_lowat`. `detail::serve_http2()`
  sets it on the connection's TCP socket (`socket_of(conn)`: over TLS, the stream's), which is where both ways of
  choosing HTTP/2 meet (ALPN in `serve_connection()`, the preface in `serve_requests()`), so never on HTTP/1.1's; a
  failure (e.g. `not_supported`) is logged at debug, and the connection's writer then doesn't wait.
- The writer (`http2_connection_base::write_loop()`; `waits_writable`, a constructor argument the server's connection
  passes, so the client's connections are as they were until part 8): a round after one that wrote first awaits
  `wait_for_socket()`, then takes the output (`fill()`, then the session's) as before. A round after one that didn't
  write doesn't wait: the last wait found the socket writable, and nothing's been written since, so only the peer's
  changed it (saving the `poll()` when the writer's woken from idle, e.g. once per request a stream at a time). Control
  frames wait with the DATA, as the plan says. A wait that fails is handled as a failed write (both now set `failed`
  and break out to one place): a `closed` condition leaves the end to the reader, anything else (e.g. `timed_out`, as
  the socket's write timeout, the server's `write_timeout`, applies to the wait) ends the connection. GOAWAY on a
  graceful drain, and the end, still go through the loop, so they wait as anything else does; the writer still shuts
  the connection down after.
- `max_ahead` is unchanged: `data_writing` (the DATA of the writer's last round) stays counted through the wait, so a
  handler's write still goes at once, without the scheduler, while less than a round's DATA is in the writer's hands
  and nothing's queued (the plan's "and meanwhile handlers' writes queue (past `max_ahead`)"). So what's past
  reordering is the mark and at most `max_ahead`, as the plan says.
- TLS (not in the plan, which only said a TLS stream's reads "then wait as a second writer would"): the socket's
  `wait_writable()` claims its write operation, so called under a TLS stream it would collide with the stream's own
  socket writes, a flush in progress (the HTTP/2 writer's last, or a read's replies: alerts, key updates, the answer to
  close_notify), whichever came second failing with `device_or_resource_busy` and, for the stream's, failing every
  later write (`write_failure`). So the wait goes through the stream's flush queue: `tls::stream::wait_writable()`
  (public, beside `shutdown()`) takes the flush on as a flush does (waiting for one in progress, as a flush waiter), waits
  for the socket, then writes what was queued meanwhile (a read's replies, which leave what they emit to a flush in
  progress) through `write_queued()`, before it returns; a wait that fails writes nothing, and fails nothing but itself.
  `detail::wait_writable(conn)` (`http/connection.hpp`) picks the stream's or the socket's. It's a coroutine (a frame
  per round over TLS, where each round's write already has one); the socket's stays a ready task when writable.
- The test hook (as planned): `detail::writable_function` (`std::function<coro::task<io::result>()>`, in
  `http2_config.hpp`, which now includes `coro/task.hpp` and `io/io.hpp`), `http2_options::writable`, set from
  `server_config::http2_writable` (internal, as `connect_options::system_connect` is), used instead of the socket only
  while the mark's set.
- Deadlines (reasoned against the plan, which wanted the test): `wrote_at`, which `waiting_turn()` reads, is set by
  `take_output()`, so it's now when the writer's back from a round's write and its wait for the socket. A stream waiting
  its turn is put off while the writer's rounds come within `write_timeout` of each other, which a slow peer that still
  reads gives; a writer held longer by the socket fails anyway (its wait times out at the same `write_timeout`, ending
  the connection), so there's nothing to put off for. Through the hook, which has no timeout of its own, a writer held
  past `write_timeout` lets queued streams time out (reset, `timed_out`): the stand-in for the connection ending. Before,
  a single `write_all()` that took longer than `write_timeout` (with progress) had the same gap; now a write and a wait
  share it.
- Tests (no network). `test/http/server_notsent_lowat.cpp` (`[http][http2][server][notsent-lowat]`): the mark read back
  by a handler (`getsockopt()` on its body writer's `native_handle()`, the connection's fd) through `http::client`: 16384
  by default and 4096 if set, over HTTP/2 with prior knowledge; 0 with the mark 0, and over HTTP/1.1. Through the hook
  (`writer_gate`: `hold()`, `grant()` a wait at a time, `open()`), with the client's windows open (16 MiB), so only the
  writer holds DATA back: a u=7 response of 256 KiB requested while the writer's held goes at once as far as
  `max_ahead` (64 KiB) and queues the rest; then u=0 and u=3 ones queue behind it (probes, which count before
  answering, as their answers wait too, through `detail::wait_queued_streams`); let go, the frames are "BBBBUUNN" then
  the rest of B's 12. Paced (a wait granted every 20 ms, so a round each, and once none for 600 ms), a u=0 response of
  4 MiB takes the connection about 2 seconds while a u=7 one waits its turn, past its `write_timeout` of 1 second: it
  isn't reset, and goes after. `test/tls/server.cpp`: over TLS with ALPN, the mark is 16384 on HTTP/2's connections and
  0 on HTTP/1.1's, and an 8 MiB response (written 64 KiB at a time) arrives whole and in order through the TLS writer's
  waits. `test/tls/stream.cpp` (`[tls][wait-writable]`): `wait_writable()` while a 12 MiB write flushes to a peer that
  reads returns, without error, once the write's gone; to one that doesn't, both time out (300 ms), never
  `device_or_resource_busy`. The existing scheduling, end-stream and waiting-turn tests now run with the mark set (the
  default) and pass unchanged. With the writer never waiting, the ordering test fails (the bulk response all goes before
  the others are requested, so it never queues); with `wrote_at` not set after a wait, the paced test's bulk response
  is reset; with the mark not set, the read-back fails; with the stream's wait going straight to the socket, the TLS
  stream test fails (macOS's `poll()` said writable while the write waited, so the wait returned at once).
- Measured (`bench_request`, Release, master's binary and the branch's alternating, on a machine busy with other work:
  six pairs at a load average of 3 to 4.5 are counted; a seventh, at a spike to 53, and three more as it decayed, are
  not). Over HTTP/2, a stream at a time: 98.4 to 122.6 us a request before (median 107.3), 105.6 to 120.4 after (median
  107.9), and 49.2 to 49.5 allocations a request before, 49.2 to 49.4 after: the `poll()` a round that wrote costs (a
  round after an idle one doesn't wait) is within the noise. 8 streams at once (256 KiB each, written 64 KiB at a time,
  over loopback, the worst case for the mark): 1426 to 1640 us a round before (median 1518, about 1320 MiB/s), 1506 to
  1682 after (median 1515, the same), and 96.3 to 97.2 allocations a response before (median 96.6), 98.6 to 99.1 after
  (median 98.8): about two more, most likely as more of the handlers' writes find the writer waiting with a round's DATA counted, so
  queue, each with its coroutine. HTTP/1.1 over a socket, untouched: 34.1 to 40.5 us before, 33.5 to 41.3 after. So
  loopback's throughput doesn't drop, and Decided question 28 stays as decided.
- Not done: part 10's relay by hand (its throwaway programs weren't kept, and the Go client and the relay would have
  to be written again), so how much this gains on a slow path is unmeasured here; the hook test is the evidence that the
  writer frames by priority while the socket's held.

### Part 5: Expect: 100-continue in the client, HTTP/1.1 (done 2026-10-21)

- As proposed (see "Expect: 100-continue in the client", Decided questions 30 to 32, all (a)), over HTTP/1.1, but for
  the decoder's flag (below). `client_options::expect_continue_timeout` (`std::chrono::milliseconds`, 1 s; 0 sends the
  body at once) sits after `io_timeout`. A request expects (`expects_continue()` in `client.cpp`) when it has a body,
  the timeout isn't 0, it's HTTP/1.1, its `Expect` has the `100-continue` token (`headers::has_token()`, any case) and
  its Content-Length (if any) isn't 0; the checks that cost nothing come first, so a request without a body pays
  nothing, and one with a body but no expectation the one lookup. HTTP/2 is untouched: its requests' bodies still go at
  once (part 6), and the 417 retry is HTTP/1.1's (it lives in `send_http11`'s path, so HTTP/2's behaviour is as before).
- The encoder: `http11::request_encode()` is now `encode_head()` (a plain function building the head's string) then
  `write_body()` (a coroutine, only called for a request with a body, so a bodyless one has no frame more), and
  `request_encode_head()` and `request_encode_body()` (in `http/http11.hpp`, internal as the header is) are those two
  halves, each checking the request as the whole does; the bytes are the same either way.
- `send_http11` writes the head alone (and flushes it) for a request that expects, then awaits `send_expecting()` (a
  coroutine of its own, so requests without the expectation have no larger frame; `send_http11` was also past
  clang-tidy's cognitive complexity with it inline). That awaits `await_continue()`: a loop that, while nothing's
  buffered, `fill()`s the connection's reader with the socket's read deadline at the continue deadline (a timeout reads
  nothing: the body goes, and `reset(nullptr)` clears the reader's sticky error), then decodes one head under
  `response_header_timeout` from its first byte. A 100 or the timeout sends the body (`send_body()`:
  `request_encode_body()` and a flush) and clears the read deadline; then the final head's decoded as before, with
  `response_header_timeout` from then. A write of the body failing with `closed` leaves the response to read, as when
  the body goes with its head (e.g. a 413 and a close); other failures fail the request. The deadline's also checked
  when a head's already buffered, so a server streaming interim responses can't hold the body off past it.
- Departure: the decoder's flag is `stop_at_interim`, not `stop_at_continue`: `response_decode()` (the overload
  borrowing its reader) returns *every* interim response (but 101) once it's passed it to `on_informational`, without a
  body, rather than only a 100. With the decoder looping past a 103 itself, the wait for the next head would run under
  whatever deadline the head was decoded with (`response_header_timeout`, 30 s), not the continue deadline the plan
  wants ("the wait goes on (the same deadline)"); and setting the continue deadline for the whole decode instead can't
  tell a timeout between heads (send the body) from one in the middle of a head (the response is broken), as a head's
  lines are taken from the reader as they're read. Returning after each interim head puts the wait back in the client's
  loop. A test shows it: a 103 and then nothing, with a 50 ms timeout, gets the body (with the plan's flag, the client
  waits for the next head for 30 s, and the test's server gives up after 5).
- Each call's heads count against `max_header_bytes` by themselves, so while the client waits, each head is limited
  alone, not with the others (the wait being bounded by `expect_continue_timeout` instead); the heads after the body are
  limited together as before. `client_request::on_informational`'s comment and the guide say so.
- A final response first: 417 (to a request whose body wasn't sent) removes the request's `Expect` (the whole field) and
  returns `sent{.retry = true}` with a placeholder error that's never returned: `send()`'s retry loop sends the request
  again at once, on a new connection (its retries take one, as a reused connection's failure suggests others have gone
  too), with `first` false, so it isn't retried again; the 417's connection is closed whatever it said, as the body its
  framing promised never came. Any other final response: Go's rule, the body's sent if the connection's kept (the
  request is HTTP/1.1 without `Connection: close`, `client_options::keep_alive` is on, and `connection_reusable()` says
  the response allows it), and the response returned either way (`deliver()`, now a plain function, attaching the body),
  the connection reused only if the body went whole. Not in the plan: HTTP/1.0 requests never wait (RFC 9110 has an
  HTTP/1.0 server ignore the expectation); and heads during the wait get `response_header_timeout` each, from their
  first byte (the plan didn't say).
- A limit (not changed): a final response first on a kept connection has the body sent before `send()` returns, while
  the response's own body is unread; a server that sends a response body larger than the sockets' buffers before reading
  the request's would stall both sides until `io_timeout` (Go writes the body on another goroutine). Servers that answer
  before reading either close (cppnet's and Go's say `Connection: close` while the 100 is owed) or read and drop the
  body, so it's left as is.
- Tests (no network). `test/http/client_expect_continue.cpp` (`[http][client][expect]`), with a body reader that notes
  its reads beside the interim responses `on_informational` sees, in order: against cppnet's server, a handler that
  reads the body gets it after the 100 (the 100 noted once, then the body's reads; the connection kept), and one that
  answers 413 without reading has its response returned, the body never read, and the connection closed (the server says
  `Connection: close`). Against raw servers the tests play (`raw_server`, a script each): a 417 to the first request
  (with and without `Connection: close`) has it sent again without `Expect` on a second connection, the first getting
  nothing after its head and the second the body whole; one that says nothing gets the body after the 50 ms wait, and so
  does one that says 103 and then nothing (the 103 passed on); a 103 then, once 200 ms pass with nothing from the
  client, a 100, gets the body after the 100 (noted 103, 100, then the reads); a 200 first keeping the connection gets
  the body, and the next request on the same connection; a 200 first with `Connection: close` gets nothing after the
  head, the body never read; a 417 once the body's been sent (after the wait) is returned; and a request without the
  expectation (a timeout of an hour), with the wait turned off (0), and with a body of 0, sends its body with its head
  (the server reads it without answering first, giving up after 5 s). Waits are the tests' own (milliseconds), and each
  test checks what arrived, not how long it took. `test/http/http11.cpp` (`[expect]`): `request_encode_head()` then
  `request_encode_body()` write what `request_encode()` does (a Content-Length, chunked with trailers, and no body), the
  head alone fails as the whole would; and `stop_at_interim` returns a 103 (passed on, its fields kept, no body), then a
  100, then the final response with its body, and doesn't stop at 101. With `expects_continue()` inverted, six of the
  nine new client tests fail (the other three are those a body sent at once also satisfies: the wait's end, the 417
  after the body, and a 200 first on a kept connection); with the 417 retry or Go's rule turned around, the 417,
  keep-alive and close cases fail; with the decoder stopping only at 100, the decoder test and the 103-then-nothing case
  fail. `test/http/client_informational.cpp`'s 100 test's comment is updated (over HTTP/1.1, the client now waits for
  that 100). `fuzz_http11` doesn't use the client's encoder or decoder, so it's unchanged.
- Measured (`bench_request`, Release, six alternating runs of master's binary and the branch's, at a load average of 2.5
  to 4.9). The client's HTTP/1.1 path, as stage 5's part 2 measured it (its HTTP/2 run with prior knowledge off, a
  throwaway change; a GET without a body, so the path without the expectation): 48.6 to 66.0 us a request before (median
  53.5), 47.9 to 57.6 after (median 54.8), 28.0 allocations a request both. The server's paths don't use what changed
  (its decoder and encoder are the requests' and responses' other halves), and are as they were: pipelined 1264 to 1458
  ns before (median 1278), 1269 to 1400 after (median 1288), 1.4 allocations; over a socket 33.1 to 68.7 us before
  (median 35.9; one run caught a spike), 34.8 to 37.0 after (median 35.9), 3.0 allocations. So nothing the noise shows,
  as expected: a request without a body pays two branches, and `write_body()`'s frame is only a body's (and directly
  awaited, so Clang may elide it).

### Part 6: Expect: 100-continue in the client, HTTP/2 (done 2026-10-21)

- As proposed (see "Expect: 100-continue in the client", Decided questions 30 to 32, all (a)), over HTTP/2, but for
  where the timer looks (below). Which requests expect is part 5's rule, now shared: `detail::expects_continue()` (in
  `http/request.hpp`, beside the server's `expectation_of()`; it was `client.cpp`'s) checks the body, the timeout, the
  `100-continue` token and a Content-Length not 0, and `send_http11` adds HTTP/1.1's version check (HTTP/2 has no
  HTTP/1.0 case). `http2_client_options::expect_continue_timeout` is the pool's `client_options` one (0 by default, so
  connections made directly, as tests do, send bodies at once). A request without a body pays nothing; one with a body,
  the one lookup, as over HTTP/1.1.
- The stream (`http2_client_stream`): `continued`, `continue_waited`, `awaiting_continue` and `continue_deadline`. The
  deadline's a plain `io::clock::time_point`, meaningful while `awaiting_continue`, rather than an `std::optional`, and
  the flags are packed with `status_code` and the stream's other flags (`head_received`, `processed`, `wants_interim`,
  moved together), so the stream's no larger: one word of status and six flags, and one of the deadline, where there
  were two words each padded around a flag or two.
- `send()`: the head goes as for any request with a body (without END_STREAM); `expect_continue()`, under the lock that
  opens the stream, sets the stream's wait, counts it in the connection's `awaiting_continue`, and wakes the timer (it
  can't see the timer's target, which is the base's, so it wakes it whatever the deadline: once per request that
  expects). Then `await_continue()`, a coroutine of its own (so `send()`'s frame only holds its awaiter), loops on the
  stream's notifier: interim heads queued for `on_informational` are passed on first, in order (without the lock); then
  a final head above 299 ends the wait without the body (a 417 setting `send_state::expectation_failed`); then the
  stream failing (reset, canceled, the connection ended) does too, `wait_for_head()` saying what came of it; then
  `continued` or `continue_waited` sends the body (`send_body()`, as before). A 2xx first waits on for the timeout (Go's
  rule, Decided question 31). A 100 and a final head above 299 found together: the final head wins, as it's the server's
  last word.
- `on_headers()`'s informational part is now `on_interim()` (`on_headers()` was past clang-tidy's cognitive complexity
  with the 100's case; `send()`'s `ends_after` is `request_ends_after()`, for the same reason): a 100 sets `continued`
  whether or not the request wants interim responses (without `on_informational`, only if the stream's waiting, which
  costs a search for `:status`).
- Departure: the timer's third deadline isn't checked by the base's `check_streams()`, which both sides share, but by
  the client's `check_connection()` (`check_continues()`), which the timer calls under the lock beside it, and which
  looks through the connection's streams only while `awaiting_continue` isn't 0. So the server's streams and timer don't
  change, and a connection without such requests (nearly every one) doesn't look. A wait past its deadline gets
  `continue_waited` and a notify; the stream doesn't fail. `stop_awaiting()` takes a stream off the count, wherever its
  wait ends: `await_continue()`, the timer, or `done()` (send()'s frame destroyed while it waits).
- Not in the plan: `done()` now resets (CANCEL) a stream whose request didn't end (`!ended`: its body withheld) as well
  as one whose response didn't. "As now" left a 403 that came with END_STREAM, before a body that's then never sent,
  half-closed (local) in the session for ever, counting against the server's MAX_CONCURRENT_STREAMS; cppnet's server
  resets such a stream itself (NO_ERROR, once its handler returns without reading), but others may not.
- 417 (Decided question 32): `send()` returns the 417 (its body attached as any response's) with `expectation_failed`
  set; `send_http2` drops it (its stream reset, as the request never ended), removes `Expect`, and returns `sent{.retry
  = true}` with `same_connection`, which has `client::send()`'s retry loop clear `tried`, so the request goes again at
  once on a new stream of the same connection (or whichever the pool gives). The plan left the connection to the retry
  loop, which would have skipped this one (a retry is "on another", for refused and dropped requests) and, as the host
  speaks HTTP/2, connected anew: a 417 says only that something doesn't do expectations, nothing of the connection, and
  a stream costs nothing. As over HTTP/1.1 it's retried whatever the attempt (the request no longer expects, so once).
  Without a `send_state` (a caller of `send()` itself, as some tests are), the 417 is returned.
- A limit (not changed): `send()` sends the body before it waits for the head, so after a 2xx first the response is
  returned only once the timeout's passed and the body's gone; Go's transport returns it at once, writing the body on
  another goroutine. A full-duplex use (the server streaming its response while reading the request) waits for the
  timeout first, which a caller wanting that doesn't set `Expect` for. And a stream refused (REFUSED_STREAM) during the
  wait, whose body's still unread, could be sent again as one refused before it's sent is; it isn't (as before, a
  request with a body isn't retried once its stream's opened).
- Tests (no network), in `test/http/client_expect_continue.cpp` (`[http][http2][client][expect]`), with part 5's `order`
  and `noting_reader`. Against cppnet's server (prior knowledge): a handler that reads the body gets it after the 100
  (noted once, then the body's reads), with and without `on_informational` (the server's `body_read_timeout` at 2 s, so
  a client that didn't see the 100 fails the test rather than sending the body after its 30 s wait); one that answers
  413 without reading has its response returned and the body never read. Against `test::h2_client` as the server (an
  `h2_host` the client connects to, through the pool): silence, with a 300 ms wait, gets nothing for 100 ms, then the
  body (the timer's check); a 103, nothing for 200 ms (the 30 s wait goes on), then a 100, gets the body after the 100
  (noted 103, 100, then the reads); a 200 first (with its body, ending the stream), with a 300 ms wait, gets nothing for
  100 ms, then the body, and the 200 returned; a 403 first has the response read whole, then its stream reset (CANCEL),
  no DATA sent; a 417 first has the request sent again on stream 3 of the same connection without `expect`, its body
  whole, and stream 1 reset without DATA; and without the expectation (a timeout of an hour), with the wait off (0), and
  with a body of 0, the body comes with the head, the server answering nothing first. Waits are the tests' own, each
  checked by what arrives. With the wait never taken, seven of the eight new tests fail (the eighth is the at-once one);
  with the timer never ending a wait, the silence and 200-first tests fail; with `continued` not set for a request
  without `on_informational`, that case fails; with `done()` not resetting a request that didn't end, the 403 test
  fails; with the retry on another connection, the 417 test fails.
- Measured (`bench_request`, Release, ten alternating runs of master's binary and the branch's, on a busy machine: load
  averages of 3.4 to 58). The HTTP/2 case, a stream at a time with cppnet's client and server (a GET, so the path
  without the expectation, which pays one branch: `has_body` is false): 108.0 to 796.0 us a request before (median
  152.7; the worst under a load of 58), 107.3 to 306.4 after (median 156.7), 48.2 to 49.4 allocations a request both
  (the client's and the server's together, varying with the connection's setup). So nothing the noise shows, as
  expected: the stream's no larger, and a connection without requests waiting has its timer look at one count.

### Part 7: the client's request bodies through the scheduler (done 2026-10-21)

- As proposed (see "Scheduling the client's request bodies", its first bullet; Decided question 33's part 7, round-robin
  until part 8). `http2_connection_base::step_write()` has one path for both roles: a write goes to the session at once,
  a frame at a time, while less than `max_ahead` of DATA waits for the writer (or is being written), no stream waits in
  the scheduler, and the output's under `max_output`; otherwise the rest waits in `send_order`, framed in its turn by
  the writer's `fill()`, and a write that doesn't wait returns a ready task. The `scheduling` flag is gone (`fill()`
  runs every round), and so are `submit_windowed()` (the client's old path, now dead) and the `drained` notifier (with
  `output_drained()`): only the client's old path waited on it, past `max_output`, where a write now queues as the
  server's do; the notifies of it (a reset, a timeout, `end()`, `cancel()`, the client's oversized interim heads) went
  with it. `hold_turn` and `waiting_turn()` come along unchanged; the client's write timeout is `io_timeout`, as
  before. Every client stream is u=3 incremental (`body_priority`, set by `open()` through `send_order.update()` before
  the stream's added), so contended bodies take turns a frame at a time; an incremental stream sharing its urgency
  never holds its turn, so the client never holds one yet.
- Not in the plan: the client makes its stream before its id is known (its head's fields view the stream's strings, and
  an opened stream must be sent), so the scheduler node's id, which the constructor takes, stayed 0:
  `scheduler_node::set_stream()` (new) sets it in `open()`. With it 0, `fill()`'s `ready` asked the session for stream
  0's window, the connection's, so a stream whose own window was used up was picked, framed nothing, and ended the
  round, holding up every other stream (a test shows it: a body without its stream's window, queued before another,
  starved the other until it timed out); and non-incremental streams (part 8's) would all have sorted as id 0.
- Not in the plan: `write_waiting()` takes its stream out of the scheduler (`unqueue()`, under the lock) if it's
  destroyed while it waits (a `util::defer`, acting only when its step isn't done). A server's handler isn't destroyed
  while it writes, but a client's `send()` may be (its `done()` already allows for that), and the queued span is in
  `send_body()`'s frame, destroyed before `send()`'s `release` calls `done()`, which would have taken it out: so the
  writer could frame from freed memory meanwhile. No test reaches it (nothing in the library destroys a task that's
  waiting), so it's by reasoning only. And `abandon()` and `end_request()` fail their stream through `fail_stream()`,
  rather than setting `failure`, so a failed stream is always out of the scheduler (neither is queued when they run,
  but that's the rule now that the client's streams can be).
- Departure from the tests the plan listed: "bodies of the same priority in rounds" is checked with the window given a
  frame at a time, each once every body with frames left has queued again (so each frame's the scheduler's pick of all
  of them): "ABCABCABC". With the whole window given at once, only the first round is fixed (each body's queued write
  goes, in the order they queued); after it, `send_body()` reads 16 KiB at a time, so each write is a frame, and a body
  whose task comes back first queues again first, and may go twice before another's back (an order such as
  "ABCAABACBBCC" was seen). The server's handlers that write a frame at a time have the same gap, which part 10's hold
  closes only for streams that would lose their turn, not for incremental ones sharing an urgency. A larger read in
  `send_body()` (64 KiB, as the server's bench writes) would keep a body queued across rounds; left for part 8 to
  weigh, with its memory per request.
- Tests (no network). `test/http/client_scheduling.cpp` (`[http][http2][client][scheduler]`), against `test::h2_client`
  as the server, unchanged (its `hold_windows` and `data_log`): a plug body of 65535 uses up the connection's window,
  which the server gives back by exact amounts (`held::give()`: consuming the plug's stream, which has ended, so only
  the connection's window grows, in one WINDOW_UPDATE), and the client's `wait_queued()` says when bodies have queued
  (a watchdog closes the connection after 10 s, so a regression fails rather than hangs). Bodies of three frames in
  rounds ("ABCABCABC", every frame whole, each body ending with its last DATA); the first round when the window opens
  at once; a body alone in whole frames (its first from the scheduler, the rest at once), ending with its last DATA of
  100 bytes; a body without its stream's window (65535 of five frames sent, never given back) not holding up one
  queued behind it, which goes whole, then going on once its window grows; a queued body timing out at `io_timeout`
  (300 ms: CANCEL, `timed_out`, nothing of it sent, the plug's request still answered); one canceled while queued with
  10,000 bytes of its write framed (CANCEL, `operation_canceled`, the server having exactly those 10,000), the other
  going on whole; and a 413 with RST_STREAM(NO_ERROR) before any body (the 413 returned, nothing of its body sent, the
  other's going whole). With the client's old path (master's sources, these tests), all seven fail (the bodies never
  queue); with the client's streams left non-incremental, the rounds ("AAABBBCCC") and the at-once tests fail; without
  `set_stream()`, the stream window test fails; without `queue_rest()` arming the deadline, the timeout test fails;
  without `cancel()` failing the stream, the cancel test fails; without `on_stream_reset()` failing it, the 413 test
  fails. The existing client tests (`[http2][client]`: end-stream, trailers, 100-continue, cancellation, flow control)
  pass unchanged. 30 runs of the new tests in a row passed.
- Measured (`bench_request`, Release, seven alternating runs of master's binary and the branch's, the new case built
  into both; one-minute load averages of 2.8 to 9.7, and one pair, whose "after" ran at a spike of 28, left out).
  The new case, 8 request bodies at once over one connection from cppnet's client to cppnet's server (256 KiB each, with
  their Content-Length, read 16 KiB at a time; the server's stream windows 64 KiB): 1655 to 2099 us a round before
  (median 1780, about 1120 MiB/s), 1444 to 1756 after (median 1664, about 1200 MiB/s), and 116.4 to 117.7 allocations
  a request before (median 116.8), 152.9 to 153.7 after (median 153.1). The 36 more are the writes that queue: with
  eight bodies contending, nearly each of a body's sixteen writes now finds another queued and waits its turn, where
  before most went at once as far as the windows allowed; each that waits costs its coroutine frame and its notifiers'
  waits (`coro::notifier::notify()` swaps its waiters' vector out, so the next wait on it allocates one again: the
  stream's, and the writer's, which the queued write wakes).
  The HTTP/2 case, a stream at a time (a GET: no body): 97.8 to 108.9 us a request before (median 104.3), 83.6 to 115.9
  after (median 101.8), 49.3 allocations a request both. The server's 8 responses at once: 1440 to 1560 us a round
  before (median 1512), 1236 to 1630 after (median 1428), 98.4 allocations a response both. So no change the noise
  shows but for the uploads, which are a little faster and allocate more.

### Part 8: the client's request bodies by priority (done 2026-10-21)

- As proposed (see "Scheduling the client's request bodies", its second bullet, and "TCP_NOTSENT_LOWAT", its last;
  Decided questions 33 (a), 28 (a) and 29 (a)). Each client stream's priority is its request's `Priority` field, read
  by `open()` as the head's made (`body_priority()` in `http2_client.cpp`, a plain function): `headers::get_all()`,
  its lines joined with `sf::join_lines()` (as the server joins them; into a local string, which allocates only when
  there are several lines), `parse_priority()` over RFC 9218's default (u=3, not incremental), as the server's
  `request_priority()` reads the same field; only a request without the field is u=3 incremental (Decided question 33:
  "u=3 incremental (round-robin) without one"). Set through `send_order.update()` before the stream's added, as part
  7's constant was. A request without the field costs the lookup (a pass over its fields, which `append_fields()` makes
  anyway), nothing allocated.
- The plan's second bullet said "over u=3 incremental", which reads as the base for the field's missing parameters
  too: then `u=1` would have been u=1 incremental for the upload but not for the response, and a field that doesn't
  parse the same as none. Decided question 33 has the field mean what it means to the server, so a field that leaves
  `i` out is not incremental (`u=1` sends its body whole before others of u=1 take theirs, by stream), and one that
  doesn't parse is RFC 9218's default whole (u=3, not incremental: `parse_priority()`'s rule), which differs from no
  field: of one urgency, the two kinds take turns.
- `client_options::http2_notsent_lowat` (16 KiB, as the server's, Decided question 28; 0 sets none), passed as
  `http2_client_options::notsent_lowat`, is set by `http2_client_connection`'s constructor (`set_mark()`, called in the
  base's initializer, on `socket_of()` of the connection it owns, so over TLS the TCP socket's): the connection's made only
  once HTTP/2's chosen (by ALPN, or prior knowledge), so never on HTTP/1.1's, as on the server. A failure's logged at
  debug and the writer then doesn't wait. If it's set, the writer waits for the socket as part 4 made the server's
  (`waits_writable`). The test hook, as the server's: `client_options::http2_writable` (internal), passed as
  `http2_client_options::writable`; the writer uses it only while the mark's set. (`client_options` now holds a
  `std::function`, so it isn't trivially copyable without TLS either: the pool moves it in both builds.)
- Tests (no network). `test/http/client_scheduling.cpp` (`[http][http2][client][scheduler][priority]`), part 7's setup
  (the plug, `held`, the client's `wait_queued()`), with `held::take_turns()` (part 7's first test's loop, now shared:
  the window a frame at a time, once every body with frames left has queued again): six bodies of two frames, `u=5, i`,
  `u=1`, `u=1`, none, `i` and `u=5` as two field lines, `u=0`, go "FFBBCCDDAEAE" (urgent first; of one urgency,
  non-incremental ones whole by stream, so a field without `i` isn't incremental, and incremental ones a frame each);
  and a field that doesn't parse (`u=0, @`) is u=3 not incremental, taking turns with a body without one (u=3
  incremental, whose kind goes first): "BABACC". `[client][notsent-lowat]`: the mark read back with
  `getsockopt()` on the response body's `native_handle()` (before the body's read, which would give an HTTP/1.1
  connection back), against cppnet's server: 16384 by default, 4096 when set, 0 with 0, and 0 over HTTP/1.1; and over
  TLS (`test/tls/server.cpp`), 16384 over ALPN's HTTP/2 and 0 over HTTP/1.1. Through the hook (`test::writer_gate`,
  moved from the server's tests to `test/http/h2_client.hpp`), against `test::h2_client` as the server with its windows
  open: once a GET has settled the settings, the writer held, a u=7 body of 8 frames goes as far as `max_ahead` (4
  frames) and queues the rest, then a u=0 and a u=3 body of a frame each queue behind it; let go, "BBBBUNBBBB". The
  checks bite: with part 7's constant priority, the six-body test, the malformed one and the hook one fail; reading only
  the first field line, the six-body one ("FFBBCCDEDEAA"); with the field over u=3 incremental, the six-body one
  ("FFBCBCDDAEAE") and the malformed one ("ABABCC"); with no field read as RFC 9218's default too, part 7's rounds
  ("AAABBBCCC") and at-once tests and the malformed one ("AABBCC"); without the mark, the read-backs (both, "2: 0") and the hook test
  (B never queues, the watchdog ends it); with the mark set but the writer not told, the hook test. The existing client
  tests pass unchanged with the mark on by default; the new ones passed 30 runs in a row.
- Measured (`bench_request`, Release, five alternating runs of master's binary and the branch's, each stopped once the
  uploads case had printed; one-minute load averages of 2.1 to 5.6. The machine slept between short maintenance wakes
  while a first attempt ran, which showed as runs taking hours and cases ten times slower: those runs are thrown away,
  and these were run holding a power assertion). The bench's uploads have no `Priority` field, so they're u=3
  incremental as before: what changes for them is the mark, now set on the client's connection, and its writer's
  waits. 8 request bodies at once (256 KiB each): 1393 to 1527 us a round before (median 1421, about 1410 MiB/s), 1413
  to 1533 after (median 1486, about 1350 MiB/s), and 152.9 to 153.4 allocations a request before (median 153.1), 151.5
  to 151.9 after (median 151.5): within the runs' spread in time, and slightly fewer allocations. Over HTTP/2, a
  stream at a time (a GET): 79.7 to 87.2 us a request before (median 83.6), 80.7 to 95.2 after (median 82.8), 49.3 to
  49.5 allocations both. The server's 8 responses at once (unchanged code, for the noise): 1210 to 1354 us a round
  before (median 1256), 1215 to 1345 after (median 1248). So the client's mark costs loopback nothing the noise shows,
  as part 4 found for the server's.

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

## Stage 6

Decided (2026-10-21). Seven things, chosen for stage 6, to finish HTTP/2. On the server: TCP_NOTSENT_LOWAT, so that
little of what's past part 10's scheduler sits in the socket's send buffer; ending a response with a Content-Length on
its last DATA frame, rather than with an empty one; and 431 for a header list over the limit, rather than a reset. On
both sides: connection coalescing and the ORIGIN frame (RFC 8336). On the client: `Expect: 100-continue` (waiting for
the 100 before sending a body); scheduling request bodies by priority, as the server does responses; and reprioritizing
a request in flight (PRIORITY_UPDATE). Extended CONNECT (RFC 8441) stays later, for WebSockets' own plan. Go's behaviour
is from its sources, read for this plan (`src/net/http/transport.go` and `src/net/http/internal/http2`, at its master of
October 2026); the kernels' from theirs (Linux's `net/ipv4/tcp.c` and `include/net/tcp.h`, XNU's
`bsd/kern/uipc_socket.c`, `uipc_socket2.c`, `bsd/netinet/tcp_subr.c` and `tcp_usrreq.c`, apple-oss-distributions' main);
curl's from its `lib/http.c`; nghttp2's from `nghttp2.h`. What's from memory says so. The pull requests they're split
into, smallest and most independent first, and why each split is where it is, are in Stages.

### Ending a stream with its last DATA

**Where things stand.** A response's stream ends in `http2_finish()`, which the server calls once the handler has
returned: with the trailers (HEADERS with END_STREAM), or an empty DATA frame with END_STREAM. So every response with a
body, Content-Length or not, ends with 9 bytes more than it needs (part 7 counted them: of the 49 bytes
`bench_request`'s second response takes, 9 are that frame), and its end waits for the handler to return. A response
without a body already ends with its head (`http2_send_head()`'s `end`), unless trailers or a `Trailer` field are set by
then. The client's requests are the same: `send_body()` ends with `end_request()`, the trailers or an empty DATA, even
when the body's length was given and there are no trailers (`make_trailers` unset).

**What RFC 9113 says.** END_STREAM goes on the last frame of the message (section 8.1); an empty DATA frame carrying it
is allowed, but is a frame the peer processes for nothing. Nothing else changes: the session already checks DATA that
ends a stream against Content-Length (on both sides), and doesn't count an empty DATA with END_STREAM as a glitch.

**How others do it.** Go's server sets END_STREAM only on what's written once the handler is done (`writeChunk`:
`endStream := rws.handlerDone && !hasNonemptyTrailers`): a response that fits its 4 KiB buffer (`handlerChunkWriteSize`)
ends with its data, flushed when the handler returns; a larger one's last write goes before, and the end is an empty
DATA, as here. nghttp2's data providers set `NGHTTP2_DATA_FLAG_EOF` on the last chunk, and hyper's server passes h2's
`send_data(data, end_of_stream)` the body's `is_end_stream()` after each chunk, so a body of known length ends with its
data in both (from memory).

**Proposed.**

- The stream learns its body's length: `http2_stream_ops::send_head` takes it (`response_writer::start()` has it, as
  `body_length`, with `framing::length`), and `http2_send_head()` sets `http2_stream_base::data_left` (an
  `std::optional<std::size_t>`, in the base, so the client's streams use it too) if the head doesn't end the stream and
  no trailers nor `Trailer` field are set: the same rule as a bodyless head's, applied when the head is sent (Open
  question 26).
- Writing (`http2_connection_base`): the frame that brings `data_left` to 0 is submitted with END_STREAM, and the stream
  marked `ended`: in `submit_at_once()` (a write that goes at once), `fill()` (one the writer frames in its turn) and
  `submit_windowed()` (the client's, until part 7 moves it onto the server's path). `http2_finish()` then finds the
  stream ended, and sends nothing. A handler that writes less than its Content-Length is reset as now (the frame that
  would end it never comes); more is refused by `response_writer`'s `limit_writer` first.
- Trailers set after the head of a response with a Content-Length are dropped (logged at debug), unless the head
  declared them (`Trailer`), as a bodyless response's are now: `response_writer::trailers()`' comment says so, and
  middleware that adds trailers after `next()` (e.g. `Server-Timing`) declares them before calling it. A response sent
  with `send(status)` (no Content-Length) is unchanged: `finish()` ends it, with the trailers or an empty DATA, as the
  server can't know which write is the last.
- The client: a request with a Content-Length and no `make_trailers` sets `data_left` when its head is sent, so its last
  DATA ends it; `end_request()` is only for trailers and bodies of unknown length.

**Costs.** None per write: a comparison on the frame that's submitted. 9 bytes and a frame less per response (and
request) with a body of known length, which `bench_request`'s bytes count shows (from the second request on, 49 bytes
down to 40).

**Tests** (no network). With `test::h2_client` (its `data_log`, and whether a frame ended the stream): a response with a
Content-Length ends on its last DATA, written at once, in several writes, and through the scheduler (the client holding
its windows, as part 10's tests do); none for a body of 0 (the head ends it, as now); a Content-Length response with
trailers set before its head, or a `Trailer` field, ends with them; trailers set after its head and undeclared are
dropped (the last DATA ended it); a handler that writes less is reset; HEAD and `send(status)` as now. The client's,
against `test::h2_client` as the server: a request with a Content-Length ends on its last DATA, one with `make_trailers`
with them, one of unknown length with an empty DATA. `fuzz_http2`'s scripts already end streams with DATA; unchanged.

### 431 for a header list over the limit

**Where things stand.** A request whose decoded header list is over the server's MAX_HEADER_LIST_SIZE
(`max_header_bytes` plus 32 a field, for up to 100 fields) is decoded to its end (HPACK's table must stay in step), and
its stream reset with PROTOCOL_ERROR by the session (`end_header_block()`: `decoded == value_too_large`), counted as a
glitch; the application never hears of it. Learned in stage 2 said the server could answer 431 instead, later. One whose
encoded block is over the limit (or its CONTINUATIONs) ends the connection, as it must; that stays. Over HTTP/1.1, a
head over `max_header_bytes` gets 431 already.

**What the RFCs say.** RFC 9113, section 10.5.1: "A server that receives a larger header section than it is willing to
handle can send an HTTP 431 (Request Header Fields Too Large) status code"; the block "MUST be processed to ensure a
consistent connection state". RFC 6585 defines 431.

**How others do it.** Go's server decodes a block over its limit with `Truncated` set, and serves the request with a
handler of its own, `handleHeaderListTooLong`, which writes 431 with a short HTML body (`processHeaders`, read for this
plan). nghttpx and h2o answer 431 too (from memory).

**Proposed** (Decided question 27).

- The session (`http2_session.cpp`): a server's request head over the limit is reported, rather than reset: the stream's
  marked as having its head (so what follows on it is trailers, and refused as a stream's), `known`, and
  `session_events::on_header_list_too_large(stream, end_stream)` (new, pure virtual: the client's connection's is a
  no-op, as its session never calls it; the tests' recorders log it) is called; it's still counted as a glitch, so a
  client that keeps sending them is stopped as now. Trailers over the limit are still reset (a response may be under
  way), and so is a response's head over the client's limit (the client fails the request as now).
- The server (`http2_server.cpp`): `refuse(id, status::REQUEST_HEADER_FIELDS_TOO_LARGE, end_stream)`, as it answers a
  method it doesn't implement (501), a malformed request (400) or an unknown expectation (417): just the status and
  `Date`, ending the stream, and RST_STREAM(NO_ERROR) if the request goes on. No handler runs (so no middleware sees it,
  as for those), and it doesn't count against the handlers' limits. A PRIORITY_UPDATE kept for the stream is dropped, as
  for a refused head.

**Costs.** None for requests within the limit; an over-long one costs a HEADERS frame where it cost a RST_STREAM.

**Tests** (no network). On bytes (`[session]`): a head over the limit reported (not reset), with and without END_STREAM,
then DATA on it ignored once the application resets it; trailers over the limit reset as before; a client's session
resetting a response's head over its limit as before; each still a glitch (the budget's pair failing the connection).
Over a socket with `test::h2_client`: a request whose fields decode to more than `max_header_bytes` gets 431 (and the
reset, if its body was to follow), and the connection goes on, its next request (indexing fields the long block added to
HPACK's table, so the tables are checked to be in step) answered.

### TCP_NOTSENT_LOWAT

**Where things stand.** Part 10's scheduler orders DATA only until it's framed: at most `max_ahead` (64 KiB) waits for
the writer, which writes each round with `io::write_all()`, returning once the socket has taken it all. What the socket
takes it holds until TCP sends it, and by hand (Learned in stage 5, part 10) macOS's autotuned send buffer held about 2
MiB of u=7 responses ahead of a u=0 one started a second later, through a 2 MB/s relay: the scheduler had nothing left
to reorder. Setting the server's send buffer small fixed it, but a small SO_SNDBUF also caps what's in flight (a long
fat path needs a window's worth), and turns off the kernel's tuning of it.

**What the kernels do.** TCP_NOTSENT_LOWAT bounds the bytes written but not yet sent, leaving what's in flight to the
congestion window:

- Linux (since 3.12): `tcp_stream_memory_free()` compares the not-sent bytes (`write_seq - snd_nxt`) with the socket's
  value (or the `net.ipv4.tcp_notsent_lowat` sysctl, which is unlimited by default). `tcp_sendmsg_locked()` checks it
  (`sk_stream_memory_free()`) before each new segment, so a non-blocking `send()` stops with EAGAIN once the not-sent
  bytes reach it (having copied into the segment under way: up to a segment's worth over); and EPOLLOUT is reported only
  once they're under half of it (the `wake` shift: "This sends EPOLLOUT only if notsent_bytes is half the limit").
- XNU (macOS, since 10.11, from memory): `send()` doesn't look at it (`sosendcheck()` checks only the buffer's space),
  so a non-blocking write still fills the whole send buffer. Only readiness does: `sowriteable()` (poll and select) and
  `filt_sowrite_common()` (kqueue's EVFILT_WRITE) call `tcp_notsent_lowat_check()`, which says writable while the
  not-sent bytes are at most the value (or, with Nagle on, under one segment); EVFILT_WRITE's `data` stays the buffer's
  free space.

So the same option needs the writer to wait for writability before it writes, on macOS; on Linux, `send()` stops by
itself. cppnet's event loops are edge-triggered (kqueue's EV_CLEAR, epoll's EPOLLET) and the scheduler keeps a
direction's `ready` flag when an edge comes with no one waiting, consumed by the next wait: a flag left from an earlier
edge may say writable when the socket isn't any more, so a wait can't stand for the level on its own.

**How others do it.** Cloudflare's tuning for nginx (Patrick Meenan, "Optimizing HTTP/2 prioritization with BBR and
tcp_notsent_lowat", 2018) sets the Linux sysctl to 16 KiB, with BBR: nginx has no setting of its own, nor does Go's
server (its writes go through a `bufio.Writer` to the socket, whatever the kernel holds). h2o's "latency optimization"
(`http2-latency-optimization-min-rtt`, 50 ms; `-max-additional-delay`, 0.1 of the RTT; `-max-cwnd`, 65535) uses
TCP_NOTSENT_LOWAT and TCP_INFO, on Linux and macOS only, to write little more than the congestion window allows, only on
connections with a long round trip and a small window (its documentation; how exactly, from memory: Kazuho Oku's 2016
slides, writing about a CWND and an MSS at a time). Apple's networking sessions recommended setting it (16 KiB, from
memory).

**Proposed** (Decided questions 28 and 29).

- `tcp_socket::set_notsent_lowat(std::size_t bytes)` (beside `set_nodelay()`, `std::expected<void, std::error_code>`, 0
  turning it off), and `socket::wait_writable()`: a task that returns once the socket is writable, by the kernel's
  reckoning (the not-sent bytes under the mark, with one set): a `poll()` that doesn't wait, for the level (both
  kernels' `poll()` apply the mark), then, if it isn't, the scheduler's write wait, and again, as an edge's stale
  `ready` flag may end the wait early. It goes through the socket's write operation, so it can't overlap a write (a TLS
  stream's reads may write their replies: they then wait as a second writer would), and its deadline, timeout and stop
  token apply.
- The server sets it on each HTTP/2 connection's socket (`server_config::http2_notsent_lowat`, 16 KiB; 0 doesn't set
  it), once HTTP/2 is chosen (by ALPN, or the preface): not on HTTP/1.1's, whose responses aren't reordered, so it would
  only cost them wakeups. Over TLS it's the TCP socket's (the ciphertext's: a record's overhead is a few dozen bytes in
  16 KiB).
- The writer (`write_loop()`), with the mark set, waits for `wait_writable()` before each round's `take_output()`, so
  the scheduler's `fill()` frames only when the socket can take more, and meanwhile handlers' writes queue (past
  `max_ahead`) for their turns. What's past reordering is then the mark (in the kernel) and a round (`max_ahead`, in the
  writer's hands), where it was the send buffer, autotuned up to megabytes. Control frames wait with the DATA: TCP would
  deliver them after what's in the send buffer anyway, so they're no later.
- The client's HTTP/2 connections get it once they schedule their request bodies (part 8): `client_options` gains the
  same, `http2_notsent_lowat`.

**Costs.** A `poll()` per writer round on HTTP/2 connections (a system call, usually writable at once), and on a fast
path (loopback, a LAN) more wakeups: the kernel holds only 16 KiB unsent, so the writer must come round before TCP
drains it (Linux wakes it at half). `bench_request`'s 8-streams-at-once case measures the throughput (over loopback, the
worst case for it), and `bench_request`'s one stream at a time the round trip; if the throughput drops much, the plan
comes back to question 28 with the numbers.

**Tests** (no network). `set_notsent_lowat()` read back with `getsockopt()` (and on an HTTP/2 connection, not on an
HTTP/1.1 one); `wait_writable()` over loopback with a peer that doesn't read: a write until EAGAIN (or, on macOS, until
`poll()` says not writable), then `wait_writable()` doesn't return until the peer reads, and returns `timed_out` at its
deadline, `operation_canceled` on its stop token. The writer's waiting can't be tested deterministically through a real
socket (what the kernel holds depends on its tuning), so the writer takes its writability wait through a hook
(`detail::http2_options::writable`, internal, as `connect_options::system_connect` is for `connect`): a test holds the
socket "unwritable", has handlers of several urgencies queue, and checks that nothing's framed meanwhile, then that, let
go, the frames come in the scheduler's order, the u=0 one first; and that the timer's write deadlines still put off a
stream waiting its turn (part 10's `waiting_turn()`). By hand, part 10's relay again: a u=0 response started a second
into u=7 ones goes out within the mark and a round, with the send buffer left to autotune.

### Expect: 100-continue in the client

**Where things stand.** The client never waits for a 100: a request that sets `Expect: 100-continue` gets the server's
100 passed to `on_informational`, but its body goes at once (Learned in stage 5, part 2). HTTP/1.1's `send_http11`
writes the head and the body in one `http11::request_encode()`, then decodes the response; HTTP/2's `send()` opens the
stream with the head, sends the body (`send_body()`), then waits for the head (`wait_for_head()`). The server's side is
done (stage 5, part 1): cppnet's server sends the 100 when the handler first reads the body, and says `Connection:
close` over HTTP/1.1 when it answers first.

**What RFC 9110 requires** (section 10.1.1). A client that will wait for a 100 "MUST send an Expect header field
containing a 100-continue expectation"; it "is not required to wait for any specific length of time", and "SHOULD NOT
wait for an indefinite period before sending the content" (an HTTP/1.0 intermediary can't pass a 100 on). A client that
receives 417 "SHOULD repeat that request without a 100-continue expectation". A server that answers before reading the
content says whether it closes the connection or goes on reading it.

**How others do it.**

- Go (read for this plan): `Transport.ExpectContinueTimeout`, "the amount of time to wait for a server's first response
  headers after fully writing the request headers if the request has an `Expect: 100-continue` header. Zero means no
  timeout and causes the body to be sent immediately": 1 s in `DefaultTransport`, 0 in a `Transport{}`. Only a caller's
  field makes it wait; it never adds one. HTTP/1.1 (`persistConn.readResponse()`, `waitForContinue()`): a 100 sends the
  body; another 1xx doesn't; a final response first sends it anyway if the connection's kept (to keep the framing), and
  skips it if the response closes it; the timeout sends it. HTTP/2 (its transport's `writeRequest`): the same wait,
  after the head; a final response above 299 aborts the body (`abortRequestBodyWrite()`: not sent, the stream not reset,
  so the response can still be read), a 1xx or 2xx doesn't (for full-duplex streams), so its body goes when the timeout
  ends the wait. 417 isn't retried.
- curl (`lib/http.c`): adds `Expect: 100-continue` itself, over HTTP/1.1 only, for a body over 1 MiB
  (`EXPECT_100_THRESHOLD`) or of unknown length; honours a caller's over any version; waits
  `CURLOPT_EXPECT_100_TIMEOUT_MS` (1 s by default, from memory); and on 417 sends the request again without it.
- hyper's client doesn't wait for a 100 (from memory).

**Proposed API** (Decided questions 30 to 32).

```cpp
struct client_options
{
    // ... as now, and:
    // How long a request with "Expect: 100-continue" (which the caller sets: the client never adds it) waits, once its
    // head is sent, for the server's first response before sending its body anyway: a 100 Continue sends it at once,
    // a final response first doesn't (but see client::send()), and a 417 has the request sent again without the
    // expectation. 0 sends the body at once, as for a request without it.
    std::chrono::milliseconds expect_continue_timeout = std::chrono::seconds{1};
};
```

**Inside.**

- Whether a request expects: its `Expect` is `100-continue` (a token of the field, in any case, as the server's
  `detail::expectation_of()` reads it), it has a body (not of length 0), and the timeout isn't 0. Otherwise, nothing
  changes.
- HTTP/1.1 (`send_http11`): `http11::request_encode()` splits into the head (`request_encode_head()`, internal: what it
  writes first now) and the body; the head is flushed, then the client waits for the first byte of a response
  (`buffered_reader::fill()` with the socket's read deadline at the timeout): none in time sends the body, and the
  response is decoded as now; some decodes heads with `http11::response_decode()`'s new `stop_at_continue`, which
  returns a 100 (passed to `on_informational` as now) rather than looping past it. A 100 sends the body and decodes
  again; other 1xx are passed on and the wait goes on (the same deadline); a final response first doesn't send the body
  unless the connection's kept (Decided question 31, Go's rule: then it's sent, for the framing), and a connection whose
  body wasn't sent isn't reused. `response_header_timeout` starts once
  the body's sent, as now.
- HTTP/2 (`http2_client_connection::send()`): the head goes without END_STREAM, then the client waits on the stream's
  notifier before `send_body()`: for a 100 (`on_headers()` sets the stream's `continued` whether or not the request has
  `on_informational`), the final head, the stream failing, or the timeout (a third deadline the timer task checks per
  stream, `continue_deadline`, which doesn't fail the stream but sets `continue_waited` and notifies it). A final head
  first: above 299, the body isn't sent and the stream's left for the server to reset (cppnet's resets with NO_ERROR
  once its handler returns without reading), or reset with CANCEL once the response is done with (`done()`, as now); a
  2xx goes on waiting for the timeout, then sends the body (Go's, for full-duplex streams).
- 417 in answer to a request that expected, whose body wasn't sent (Decided question 32): sent again at once, once,
  without the expectation (its `Expect` removed), on whatever connection `send()`'s retry loop takes: the body's reader
  is untouched, so no `get_body` is needed. A 417 after the body was sent (the timeout passed first) is returned.

**Costs.** None for requests without the expectation (a lookup of `Expect` among their fields, done once). One that
expects costs a flush of its head on its own over HTTP/1.1 (two writes where there was one), and a round trip (or the
timeout) before its body: what it asks for.

**Tests** (no network). Against cppnet's server, over both versions: a handler that reads the body gets it after the 100
(`on_informational` seeing the 100 first, once); one that answers 413 without reading: the response returned, the body
never read (a body reader that counts its reads), over HTTP/1.1 the connection not reused. A 417 (from a raw HTTP/1.1
server the test plays, to the first request) has the request sent again without `Expect`, the second's body arriving
whole. Against servers the tests play (raw HTTP/1.1, and `test::h2_client` as the server), with a short
`expect_continue_timeout`: one that never answers gets the body after the timeout; a 103 then a 100 (the body after the
100, the 103 passed on); a 200 first over HTTP/1.1 keep-alive (the body sent, the connection reused) and with
`Connection: close` (not sent); over HTTP/2 a 200 first (the body after the timeout) and a 403 first (none, the response
read whole). Timeouts are the tests' own (milliseconds), each checked by what arrives, not by time.

### Scheduling the client's request bodies

**Where things stand.** Part 10's scheduler is the server's only (`http2_connection_base::scheduling`, from the
session's role): a client's request body is submitted to the session as far as the windows allow (`submit_windowed()`),
and waits for them to grow (or, past `max_output` of output, for the writer), as Go's client does. So bodies sent at
once on a connection go in whatever order their tasks submit, each as much as its window allows, with no priority; and
each body write that has to wait is a coroutine.

**How others do it.** Go's client writes request bodies as flow control allows (`writeRequestBody()`), with no order;
none of the clients looked at for stage 5 (hyper, curl, Chrome, Firefox) orders uploads by RFC 9218, which describes the
response's priority (from memory for the browsers). So this is cppnet's own.

**Proposed** (Decided question 33).

- The client's streams take the server's path (`scheduling` for both roles): a write goes to the session at once while
  less than `max_ahead` waits for the writer and no stream waits its turn, and otherwise waits in the scheduler, framed
  in its turn by the writer's `fill()`; a write that doesn't wait returns a ready task, as the server's do. The turn
  hold (`hold_turn`) and the write deadlines put off while waiting a turn (`waiting_turn()`) come with it: the client's
  write timeout is `io_timeout`, as now. With every stream u=3 incremental, contended bodies share the connection a
  frame at a time.
- Then each stream's priority is its request's `Priority` field (`parse_priority()` over u=3 incremental, the server's
  "unprioritized"; RFC 9218 has no other signal of a request's), read when the head's sent (`open()`), and changed by
  `set_priority()` (below).

**Costs.** Per stream, nothing more (the scheduler's node is in `http2_stream_base` already). A body write that doesn't
have to wait no longer needs a coroutine frame (as the server's responses gained in part 10); one that does, through the
scheduler, is woken once it's framed. `bench_request`'s HTTP/2 case (a request without a body) is unchanged; a new case
sends 8 bodies at once from cppnet's client.

**Tests** (no network). Against `test::h2_client` as the server, holding its windows (`hold_windows`) so bodies contend,
then letting them go (its `data_log` in order): bodies of the same priority in rounds; then by their requests'
`Priority` fields, urgent first, non-incremental by stream id; a body alone in whole frames; a queued body timing out
(`io_timeout`), and one whose request is canceled (`request.stop`) while queued, its write returning what was framed and
the others going on; the server resetting a queued stream (413 before the body). `fuzz_http2` is unchanged (its scripts
submit through the session).

### Reprioritizing a request in flight

**Where things stand.** The client sends no PRIORITY_UPDATE: a caller sets a request's priority with its `Priority`
field (stage 5's Decided question 24 (a)), and nothing can change it once `send()` has it; (c) was left for later, as it
needs a handle on a request in flight. The client's session takes a PRIORITY_UPDATE as PROTOCOL_ERROR (only servers
receive them), but has no way to send one.

**What RFC 9218 says** (section 7.1). A client sends PRIORITY_UPDATE on stream 0, for a request's stream, with a
complete set (what it leaves out is the default), at any time; the server applies it, if it can, over the request's
field (the most recent signal wins). Servers bound what they keep and count (nghttp2 counts every PRIORITY_UPDATE as a
glitch; Go's server, one that doesn't parse as a stream error: stage 5's How others do it).

**How others do it.** Firefox sends PRIORITY_UPDATE when it reprioritizes (from memory, as in stage 5), Chrome only over
HTTP/3 (from memory); Go's, curl's and hyper's clients send none.

**Proposed API** (Decided question 34).

```cpp
struct client_response
{
    // ... as now, and:
    // set_priority asks the server to send the rest of the response with priority p (RFC 9218): over HTTP/2, a
    // PRIORITY_UPDATE frame for its stream (and the request's own body, if it's still being sent, takes p's turns).
    // It returns false, sending nothing, over HTTP/1.1, or once the response has ended (or failed). The server may
    // ignore it. Any thread may call it while the body is being read.
    bool set_priority(const priority& p);
};
```

**Inside.**

- The session: `submit_priority_update(stream, priority)` (a client's only; a server's fails with `invalid_argument`):
  the frame, with `to_string(p)` as its value, for a stream the client has open (otherwise nothing, returning false).
  Only what changes is sent: the stream keeps what it last said (initially the request's field's), and an update that's
  the same isn't sent; one made while the last hasn't been taken by the writer replaces it in place (so a caller calling
  it in a loop costs a frame per writer round at most, not a glitch per call at a server that counts them, as
  nghttp2's).
- `client_response::set_priority()` asks the body, as `trailers()` does: `detail::trailer_source` becomes
  `detail::body_source` (its `trailers()` and a virtual `set_priority()`), found with a `dynamic_cast`, so nothing's
  added to `client_response`; `pooled_body` (HTTP/1.1) returns false, `http2_response_body` takes the connection's lock,
  moves the stream in the client's scheduler (`send_order.update()`, after part 8) and submits the frame, then wakes the
  writer.

**Costs.** Nothing unless called: no member, nothing allocated; a call costs the lock, a frame of about 20 bytes and a
writer wakeup.

**Tests** (no network). On bytes: a client's session's PRIORITY_UPDATE (stream 0, the stream's id, the value), none for
a closed or unknown stream, none for a repeat, one replacing another before the output's taken; a server's refusing to
send one. Against cppnet's server (`detail::stream_priority(req)` in the handler, part 9's): a response's priority
changed from the client mid-body (the handler waiting at a gate for it), and its DATA then in the new order against
another response (part 10's scheduling tests' setup); false over HTTP/1.1 and once the response has ended; an update
while the request's body is still queued moving it (after part 8).

### Connection coalescing and the ORIGIN frame

**Where things stand.**

- The client's pool keys connections by scheme, host and port (`make_key()`), and a request uses only its own key's: a
  host whose name differs gets a connection of its own, even to the same server, with a certificate covering both.
- ORIGIN (frame type 0xc) is ignored by both sides' sessions, as an unknown frame type. A 421 is a response as any other
  (`status::MISDIRECTED_REQUEST` exists): the client returns it.
- `tls::stream` verifies the server's certificate for the host it connected to (`tls_verify_cert_chain`, with Botan's
  `x509_path_validate()` and that host), and keeps the chain (`peer_certificates()`).

**What the RFCs require.**

- RFC 9113, section 9.1.1: a connection may be reused for requests with other authorities "as long as the origin server
  is authoritative"; without TLS, that depends on the host having resolved to the same address; for https, also on "a
  certificate that is valid for the host in the URI", which "MUST satisfy any checks that the client would perform when
  forming a new TLS connection for the host". A server that isn't authoritative sends 421.
- RFC 9110, section 15.5.20: on 421, a client "MAY retry the request -- whether or not the request method is idempotent
  -- over a different connection".
- RFC 8336 (ORIGIN): the frame is on stream 0 (otherwise ignored), has no flags (a frame with any of the four low ones
  set is ignored), and its payload is entries of a 16-bit length and an origin's ASCII serialization ("If parsing fails,
  the field MUST be ignored"); it MUST be ignored over cleartext (h2c), by a server, and from a proxy; an intermediary
  doesn't forward it. A client's Origin Set for a connection starts uninitialized (RFC 9113's rules apply); the first
  ORIGIN frame initializes it with the initial origin (https, the SNI host lowercased, the connection's port) and its
  entries, and later ones add to it. A client "MUST NOT consider a connection to be authoritative for an origin not
  present" in an initialized set, "SHOULD use the connection for all requests to origins in" it, still checking the
  certificate, and "MAY avoid consulting DNS"; on 421 it MUST remove the request's origin from the set. Its security
  considerations (section 4): a client that skips DNS lets "an attacker who possesses a valid certificate" take requests
  without being on the path, so such clients "ought to employ some alternative means" of confidence in the certificate,
  e.g. proof of Certificate Transparency or a recent OCSP response. The set's size is unbounded by the RFC.

**How others do it.**

- Go's client doesn't coalesce: its HTTP/2 pool is keyed by authority, and neither 421 nor ORIGIN appears in its
  transport (read for this plan). Nor do hyper's and curl's (from memory). OkHttp coalesces by address and certificate
  (from memory).
- Chrome coalesces by address only: DNS must return the address the connection was made to, and the certificate cover
  the host; Firefox by overlapping address sets (transitively) and ORIGIN, the only browser that supports it
  (Singanamalla et al., "Respect the ORIGIN!", IMC 2022, which also found no server sending ORIGIN in the wild then).
- nghttp2's library sends and receives ORIGIN (`nghttp2_submit_origin()`, `NGHTTP2_ORIGIN`); Cloudflare sent it for that
  paper's experiment; nginx and Go's server don't.

**Proposed** (Decided questions 35 to 40).

- The session (`http2.hpp`, `http2_session.hpp`): `frame_type::origin` (0xc). A server's session sends one
  (`submit_origin(origins)`, split into frames of at most the peer's MAX_FRAME_SIZE), and ignores any it receives; a
  client's ignores one on a stream other than 0 or with a reserved flag, skips an entry whose length runs past the
  payload (and the rest of the frame), and reports the entries to `session_events::on_origin()` (new, pure virtual,
  taking a span of views: the server's connection's is a no-op).
- The server: `server_config::http2_origins` (a `std::vector<std::string>`, empty by default), each an https origin
  (`https://host` or `https://host:port`, normalized to lowercase and without the default port), checked when the server
  is made against its TLS context's certificate (one it doesn't cover is refused, `std::invalid_argument`, as clients
  would ignore it), and sent in an ORIGIN frame (more, if they don't fit one) after the server's SETTINGS, over TLS only
  (never over cleartext, where RFC 8336 has it ignored). Answering 421 to a request for an origin the application
  doesn't serve is the application's (`rw.send(status::MISDIRECTED_REQUEST, 0)`): the guide says so.
- The client's connections: an `http2_client_connection` over TLS keeps its Origin Set (uninitialized until the first
  ORIGIN; then the initial origin, the key's host and port, and the entries, at most 256, the rest ignored), and
  remembers, per host, whether its certificate is valid for it (Decided question 37: the whole verification again, as
  `tls_verify_cert_chain` with that host, through `tls::detail::verify_for(stream, host)`, internal, which the
  handshake's verification shares), so it's checked once per host and connection. Never with
  `insecure_skip_verification` (nothing establishes authority then), nor over cleartext.
- The pool (`client_options::http2_coalescing`, off by default: Decided question 36): a request whose own key has no
  connection with a stream free looks, before connecting, through the pool's list of HTTP/2 connections whose Origin Set
  is initialized (`coalescing`, kept as the connections' callbacks report ORIGIN and their end) for one whose set has
  the request's origin, whose certificate is valid for its host, and which has a stream free (reserved as
  `available_http2()` does). Connections stay under their own keys (Decided question 38): per-host limits, idle
  timeouts, eviction and `http2_hosts` count them as now, and the lease says it's `coalesced`.
- 421 (Decided question 40): a coalesced request's 421 removes its origin from that connection's set (RFC 8336's MUST),
  the response is discarded (as a redirect's, `discard()`), and the request is sent again, once, on a connection of its
  own key, not coalesced, if it can be (no body, or `get_body` to read it again, as for a 307); otherwise the 421 is
  returned. A 421 on a connection of the request's own key is returned as it is.
- Coalescing by address (RFC 9113's rule for a connection without ORIGIN, Chrome's) is Decided question 35's (b), not
  chosen; it would be a part of its own: the pool resolves a new host's name first when a connection whose
  certificate covers it exists, compares the addresses with the connection's peer's, and otherwise connects to the
  addresses it resolved (`tcp_socket::connect()` with the addresses, so the name isn't resolved twice, giving up the
  start on the first family's addresses that stage 7 of the resolver's plan made, for those hosts only).

**Costs.** Nothing for a pool without coalescing on, nor for a request whose own key has a connection free; with it on,
a request that would connect looks through the coalescing connections first (a few: each is a connection the pool
holds), and a host's first coalesced request costs a certificate verification (signatures again: about what a
handshake's verification costs, once per host and connection). A server's ORIGIN frame is sent once per connection.

**Tests** (no network). On bytes: the server's session's ORIGIN (entries, split across frames), ignored by a server's
session; a client's reporting entries, ignoring one on a stream, one with a reserved flag, and a malformed entry (the
rest of the frame dropped). The server, with `test::h2_client` over a `tls::stream`: its ORIGIN frame after SETTINGS
with the configured origins, none over cleartext, none when the list is empty; an origin its certificate doesn't cover
refused at construction. The client, against a cppnet server over TLS on loopback (its certificate for `a.test` and
`b.test`, made by `test/tls/test_pki.hpp`, and ORIGIN for both; a `dns::resolver` whose injected configuration's hosts
table names both 127.0.0.1): with coalescing on, a request to `b.test` after one to `a.test` goes on the same connection
(the pool's stats: one connection), but not without ORIGIN, not with coalescing off, not for an origin not in the set,
not for a host the certificate doesn't cover even if listed (by a server the test plays, `test::h2_client` in its server
role over a `tls::stream`, as cppnet's server refuses to list it), not with `insecure_skip_verification`; a handler
answering 421 to `b.test` has the request sent again on a connection of its own (two connections), the origin then gone
from the first's set, and a POST with `get_body` sent again, one without it returned as 421. The certificate check alone
(`tls::detail::verify_for()`): a name the certificate has, a wildcard's, one it doesn't, an IP address, and an expired
certificate.

## Stages

1. **HPACK.** The static table, Huffman coding (the nibble decoder generated at compile time), integers and strings,
   the dynamic table, the decoder (with its limits) and encoder. Tests (Appendix C, hpack-test-case) and a fuzz
   target.
2. **Frames and the connection's state machine**, without I/O: the codec, settings, streams' states, flow control,
   errors, GOAWAY. Tests on bytes, and a fuzz target. The stub (and `util/packed.hpp`) is removed.
3. **The server.** The new primitive in `coro/` (`coro::notifier`), with its tests and the benchmark against `wait_on`
   (see Decided and Learned in stage 3), first; then `response_writer`'s back ends; the connection coroutine, its writer
   and the handlers' tasks; ALPN and prior knowledge; per-stream timeouts; the limits; graceful `close()`. Loopback
   tests (with a minimal test client), h2spec, curl, h2load.
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
   4. **Trailers on the server.** Done (2026-10-20; see Learned in stage 5). `server_request::trailers`,
      `response_writer::trailers()`, over HTTP/1.1 and HTTP/2 (whose session already handles them); tests with raw
      requests and `test::h2_client`. One side, both versions: the API is the same over either, and the server's tests
      don't need the client's part.
   5. **Trailers in the client.** Done (2026-10-20; see Learned in stage 5). `client_response::trailers()` and
      `client_request::make_trailers`, over both versions; tested against 4's server, so after it.
   6. **HPACK's table, built.** Done (2026-10-20; see Learned in stage 5). `dynamic_table`'s storage (no allocation per
      entry: the decoder gains at once), the encoder's table, search, eviction and size updates, and `hpack::indexing`,
      with HTTP/2's sessions on `none`, so nothing changes on the wire; `fuzz_hpack`'s new properties (with
      `automatic`). Split from 7 so the mechanics are proved on their own, by the fuzzer, before anything depends on
      them.
   7. **HPACK's indexing, on.** Done (2026-10-21; see Learned in stage 5). The policy (Decided questions 13 and 14),
      the encoder's cap (`encoder_table_size`), the sessions indexing; `bench_request`'s bytes per request; curl and
      h2load by hand. A wire change, so its own pull request, to measure and to revert alone.
   8. **Structured Fields and `http::priority`.** Done (2026-10-21; see Learned in stage 5). RFC 9651's Dictionary
      parsing, `parse_priority()`, `to_string()`, `fuzz_structured_fields`. Without I/O, and nothing uses it yet: split
      out so the parser is reviewed (and fuzzed) on its own.
   9. **Priority signals.** Done (2026-10-21; see Learned in stage 5). The session's PRIORITY_UPDATE and
      SETTINGS_NO_RFC7540_PRIORITIES, `on_priority_update`, and each server stream's priority (the request's field,
      updates, the response's field), recorded but not yet acted on; tests on bytes. Split from 10 as the protocol's
      rules are tested on bytes, apart from the writer. (`server_config::http2_priorities` came with it, rather than
      with 10: what it turns off is recorded here.)
   10. **Scheduling by priority.** Done (2026-10-21; see Learned in stage 5). The scheduler, the connection's contended
      path; tests over a socket, and `bench_request` with streams that contend. Last: it changes the writer every HTTP/2
      response goes through, so it comes once the rest is in, and alone.

   Trailers come before HPACK and priorities as they're what a user needs (gRPC); HPACK's two parts, internal, before
   priorities' three, the largest change.

6. **Decided (2026-10-21), in twelve pull requests, in this order** (see "Stage 6"; each mergeable and tested alone):
   1. **Ending streams with their last DATA.** Done (2026-10-21; see Learned in stage 6). A body of known length ends
      with its last DATA frame, on both sides: `http2_stream_base::data_left`, set from the response's Content-Length
      (`http2_stream_ops::send_head` takes it) and the request's; trailers set after the head of such a response
      dropped unless declared (Decided question 26). First: small, on the wire alone (9 bytes a response), and
      independent of the rest.
   2. **431 for a header list over the limit.** Done (2026-10-21; see Learned in stage 6). The session reports a request's head over MAX_HEADER_LIST_SIZE
      (`on_header_list_too_large()`) rather than resetting it, and the server answers it with `refuse()`; tests on bytes
      and with `test::h2_client`. Small, the server's only, and apart from the writer.
   3. **TCP_NOTSENT_LOWAT, the socket's part.** Done (2026-10-21; see Learned in stage 6).
      `tcp_socket::set_notsent_lowat()` and `socket::wait_writable()` (a
      `poll()` for the level, then the scheduler's write wait), tested over loopback sockets. Split from 4 so the I/O
      layer's part, which differs between the kernels, is reviewed and tested on its own.
   4. **TCP_NOTSENT_LOWAT, the server's writer.** Done (2026-10-21; see Learned in stage 6).
      `server_config::http2_notsent_lowat` (Decided question 28) set on each
      HTTP/2 connection, and the writer waiting for writability before each round (Decided question 29), through a test
      hook; `bench_request` with streams that contend, and part 10's relay by hand. It changes the writer every HTTP/2
      response goes through, so it's alone.
   5. **Expect: 100-continue, HTTP/1.1.** Done (2026-10-21; see Learned in stage 6).
      `client_options::expect_continue_timeout` (Decided question 30), the head flushed apart from the body,
      `response_decode()`'s `stop_at_continue` (built as `stop_at_interim`), a final response first (Decided question
      31), and 417 sent again without the expectation (Decided question 32); tests against cppnet's server and raw ones.
      The API comes with this part, over the version that's simpler to get right.
   6. **Expect: 100-continue, HTTP/2.** Done (2026-10-21; see Learned in stage 6). The stream's `continued` and
      `continue_deadline` (the timer's third deadline), `send()` waiting before `send_body()`, and HTTP/2's rule for a
      final response first; tests against cppnet's server and `test::h2_client` as the server. After 5, whose API and
      417 handling it shares.
   7. **The client's request bodies through the scheduler.** Done (2026-10-21; see Learned in stage 6). The client's
      streams on the server's write path
      (`scheduling` for both roles: at once when uncontended, otherwise in turns; writes that don't wait without a
      coroutine), every stream u=3 incremental, so contended bodies take turns a frame at a time; `bench_request` with 8
      bodies at once. Split from 8 so the move onto the shared path is measured and reviewed without priorities.
   8. **The client's request bodies by priority.** Done (2026-10-21; see Learned in stage 6). Each stream's priority
      from its request's `Priority` field (Open question 33), and `client_options::http2_notsent_lowat` for the client's connections (part 4's writer); tests with
      bodies of several urgencies. Small, once 7 is in.
   9. **Reprioritizing a request in flight.** The session's `submit_priority_update()` (a client's: only what changes,
      at most one pending a stream), `client_response::set_priority()` (Decided question 34) through
      `detail::body_source`, moving the stream's own body in the client's scheduler too; tests on bytes and against
      cppnet's server. After 8, whose scheduler it updates.
   10. **ORIGIN, the session and the server.** `frame_type::origin`, `submit_origin()`, `on_origin()` (a client's
       session), and `server_config::http2_origins` checked against the certificate and sent over TLS (Decided question
       39); tests on bytes and with `test::h2_client`. First of coalescing's three, as the server's side stands alone
       (other clients, Firefox's, use it).
   11. **The client's origin sets.** Each TLS connection's Origin Set from ORIGIN frames, and the certificate checked
       for another host (`tls::detail::verify_for()`, Decided question 37), recorded but not yet used by the pool; tests
       of the sets (from `test::h2_client` as the server) and the check alone. Split from 12 as part 9 of stage 5 was
       from 10: the protocol's and TLS's rules tested apart from the pool.
   12. **Coalescing in the pool, and 421.** `client_options::http2_coalescing` (Decided questions 35 and 36), the pool's
       list of connections with origin sets, searched before connecting (Decided question 38), and a coalesced request's
       421 (Decided question 40); tests with two names on loopback servers. Last: it changes how the pool picks
       connections, which every request goes through.

   Server-side fixes first (small, independent), then the client's 100-continue (an API users ask for), its bodies and
   priorities (building on part 10's scheduler), and coalescing last (the largest, and the pool's).

   Still later: extended CONNECT (RFC 8441), which needs a plan for WebSockets first; and, if the measurements of part 4
   call for it, h2o's adaptive mark (from TCP_INFO's round trip and congestion window).

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

## Decided questions (stage 6)

All decided as recommended (2026-10-21); the options are kept for the reasoning.

26. **Trailers on a response with a Content-Length, over HTTP/2.** (a) The stream ends with its last DATA unless
    trailers or a `Trailer` field are set when its head is sent (the rule a bodyless response follows now); trailers set
    later, undeclared, are dropped; (b) the last frame is held back (copied into the stream, up to MAX_FRAME_SIZE) until
    `finish()`, which sends it with END_STREAM or follows it with the trailers, so they may be set to the end (as Go's
    buffered last 4 KiB in effect are); (c) as now: `finish()` always ends it, with the trailers or an empty DATA.
    **Recommended: (a)**: the response is complete as soon as its last byte is, rather than when the handler returns; no
    copy; and it's the rule the API already states for a response without a body. What it costs is middleware adding a
    trailer after `next()` to a Content-Length response: it declares it first (`Trailer`), as RFC 9110 says a sender
    should. (b) delays every such response's last frame until the handler returns, to keep a rare case working.
27. **431's form.** (a) The server answers it as it does a 400, 417 or 501 it refuses: the status and `Date`, no body,
    no handler (so no middleware), RST_STREAM(NO_ERROR) if the request goes on, still counted as a glitch; (b) Go's: a
    handler of the server's own, with a short HTML body, run as handlers are (counted against their limits); (c) as now:
    the stream reset (PROTOCOL_ERROR). **Recommended: (a)**: what the RFC suggests, the same over both versions
    (HTTP/1.1 answers 431 before the handler too), and cheap; the glitch keeps a client that sends nothing else from
    costing more than a reset did.
28. **TCP_NOTSENT_LOWAT's value, and where.** (a) Per HTTP/2 connection (not HTTP/1.1's), 16 KiB
    (`server_config::http2_notsent_lowat`, 0 off), on by default; (b) h2o's: adaptive, from TCP_INFO (only on paths with
    a long round trip and a small congestion window), on Linux and macOS; (c) off by default, leaving it to the system
    (Linux's `net.ipv4.tcp_notsent_lowat`, as Cloudflare's tuning sets for nginx; macOS has no such setting).
    **Recommended: (a)**: 16 KiB is the value Cloudflare tuned to (and Apple suggested, from memory), and per connection
    reaches macOS, which has no system-wide setting, without touching HTTP/1.1; it only bounds what's not yet sent, not
    what's in flight, so a long fat path still fills its window. If part 4's measurements show it costs loopback
    throughput much, the plan comes back with them (to (b), or a larger value).
29. **How the writer waits for writability.** (a) Alike on both kernels: a `poll()` that doesn't wait before each round,
    then the scheduler's write wait if it says no (`socket::wait_writable()`); (b) by kernel: Linux's `send()` stops at
    the mark by itself (EAGAIN), so only macOS checks; (c) each round cut to the free space the kernel reports
    (EVFILT_WRITE's `data`, Linux's SIOCOUTQNSD). **Recommended: (a)**: one path, tested on both; with (b) Linux's
    writer would frame a round while the kernel's at the mark (it wakes at half of it), and save only the `poll()`;
    (c)'s kqueue `data` is the buffer's space, not the mark's, and the edge-triggered registrations would need a second,
    level-triggered one.
30. **Expect: 100-continue's default.** (a) `client_options::expect_continue_timeout`, 1 s, waiting whenever the caller
    sets the field on a request with a body; the client never adds it (Go's `DefaultTransport`, curl's timeout); (b) 0
    by default: the body at once, as now, unless the timeout is set (Go's zero `Transport`); (c) (a), and the client
    adds the field itself over HTTP/1.1 for bodies over 1 MiB or of unknown length (curl's). **Recommended: (a)**: a
    caller that sets the field asks for the wait, and today gets nothing for it; (c) would make every large upload wait
    a round trip, on a guess that servers refuse it.
31. **A final response before the 100.** (a) Go's: over HTTP/1.1, the body sent if the connection's kept (its framing
    needs it) and not if the response closes it; over HTTP/2, no body for a status above 299 (the stream left for the
    server to reset, or reset once the response is done), and the body after the timeout for a 2xx (a full-duplex stream
    may want it); (b) never sent (over HTTP/1.1, the connection then closed); (c) sent once the timeout passes, whatever
    the status, unless the server reset the stream or closes the connection. **Recommended: (a)**: what the most used
    client does, keeping HTTP/1.1 connections usable and not cutting off a server that answers 200 before reading (as
    cppnet's server does for a handler that sends its head first); (b) would hang such a server's handler until its body
    read timeout.
32. **417.** (a) The request sent again at once, once, without the expectation, if its body wasn't sent (RFC 9110's
    SHOULD; curl's): its reader's untouched, so no `get_body` is needed; (b) the 417 returned (Go's). **Recommended:
    (a)**: a 417 says only that something on the way doesn't do expectations, which the caller can't fix but by sending
    again without, as (a) does for it.
33. **What orders the client's request bodies.** (a) The request's `Priority` field (its response's priority, RFC 9218's
    only signal of a request's), u=3 incremental (round-robin) without one, as the server takes a client that never
    signals; (b) round-robin always, part 7 alone (fair shares, no priorities); (c) a priority of the client's own for
    uploads (a `client_request` member). **Recommended: (a)**: a caller that marks a request background (u=7) means its
    upload too, nothing new to learn, and without a field it's (b) anyway.
34. **The handle for reprioritizing.** (a) `client_response::set_priority(priority)`, once `send()` has returned (the
    response's head is in), through the body (as `trailers()`: nothing added to `client_response`), false over HTTP/1.1
    or once the response's done; (b) a handle made before sending (a `client_request` member sharing state with the
    caller, as `std::stop_source` does), usable from the request's head on; (c) both. **Recommended: (a)**: the server's
    priority orders the response's DATA, which only flows once its head is sent, so (a) reaches nearly all of what an
    update can change; (b) costs a shared allocation per request that uses it and state kept in step across threads, for
    the time before the head.
35. **Which coalescing.** (a) By ORIGIN only: a connection whose Origin Set the server initialized takes requests for
    its origins, the certificate checked, without DNS (RFC 8336's MAY; Firefox's ORIGIN path); without ORIGIN, none; (b)
    (a), and by address for connections without ORIGIN (RFC 9113's rule: the host resolves to the connection's peer
    address; Chrome's, Firefox's, OkHttp's), a thirteenth part; (c) by address only (Chrome's), ORIGIN ignored.
    **Recommended: (a)**: the server says what it serves, so nothing is guessed from addresses, and the pool's
    connecting (and the resolver's Happy Eyeballs start, stage 7 of its plan) stays as it is; by address helps mostly
    browsers, whose pages pull from many names on one CDN, and costs a lookup before every new host's connection (when a
    covering connection exists). Go's, hyper's and curl's clients coalesce not at all.
36. **Coalescing on by default.** (a) Off: `client_options::http2_coalescing = true` turns it on; (b) on (Firefox's).
    **Recommended: (a)**: RFC 8336's section 4 warns that a client skipping DNS lets whoever holds a valid certificate
    for a name (mis-issued, or a stolen key) take its requests without being on the path, unless it checks Certificate
    Transparency or revocation, which cppnet doesn't (its policy doesn't require revocation data); and a library client
    often fetches URLs others choose (webhooks, crawlers) in the same process as its own credentialed calls, which a
    hostile server listing their origin could then receive. Those who want it, for a CDN's many names, turn it on.
37. **The certificate check for a coalesced host.** (a) The whole verification again for that host, as a new
    connection's handshake would do it (trust, validity now, the name: `tls_verify_cert_chain` with the host, as RFC
    9113 requires), once per host and connection; (b) the name only (Botan's `matches_dns_name()`), the chain having
    been verified at the handshake. **Recommended: (a)**: the RFC's MUST ("any checks that the client would perform when
    forming a new TLS connection"), and it catches a certificate that expired since the handshake; once per host, so its
    signatures cost about a handshake's verification, once.
38. **How the pool finds coalesced connections.** (a) Each stays under its own key; a pool-wide list of the connections
    with an Origin Set is looked through when a request's own key has none free, before connecting; (b) a connection is
    registered under each of its origins' keys too. **Recommended: (a)**: per-host limits, idle timeouts, eviction and
    the HTTP/2 hosts the pool remembers stay keyed as now, and the list is short (connections the pool holds); (b) would
    have every one of them count a connection under several hosts.
39. **What the server lists in ORIGIN.** (a) `server_config::http2_origins`, explicit, each checked against the
    certificate when the server's made (refused if it doesn't cover it), sent over TLS only; (b) the certificate's DNS
    names (but wildcards, which ORIGIN can't say), with the listener's port; (c) (a), unchecked. **Recommended: (a)**:
    only the application knows which names it serves (a certificate often covers more), and an origin the certificate
    doesn't cover is one clients ignore, so it's a mistake to catch early; (b) would advertise names the routes don't
    serve, each costing a 421.
40. **421 in the client.** (a) A coalesced request's 421 removes its origin from that connection's set (RFC 8336's MUST)
    and has the request sent again, once, on a connection of its own key, not coalesced (with its body only through
    `get_body`, as a 307's; otherwise the 421 is returned); a 421 on a connection of its own key is returned; (b) any
    421 sent again once, on a new connection. **Recommended: (a)**: a 421 on a coalesced connection is the pool's doing,
    so the pool undoes it; on the request's own connection, a new one to the same host is likely to reach the same
    server and say the same, and RFC 9110 only allows the retry (MAY).
41. **The parts' order.** (a) As in Stages: the server's fixes, then 100-continue, then the client's bodies and
    priorities, then coalescing; (b) coalescing before the client's scheduling (it's independent of it). **Recommended:
    (a)**: smallest and most independent first, and the pool's change, the largest and the one every request goes
    through, last; (b) is fine too if coalescing matters sooner.

## Out of scope

Server push (browsers dropped it), `Upgrade: h2c` (RFC 9113 dropped it), RFC 7540's priority tree (deprecated; its
frames are parsed and ignored), and HTTP/3 (parked: [http3.md](http3.md)).
