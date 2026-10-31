# Plan: WebSockets

Status: decided (2026-10-22): every question at the end as recommended. Stage 1, the helpers, done (2026-10-24; see
Learned in stage 1). Stage 2, the codec and the session, done (2026-10-29; see Learned in stage 2). Stage 3, switching
an HTTP/1.1 connection, done (2026-10-29; see Learned in stage 3). Stage 4, accepting WebSockets, done (2026-10-29; see
Learned in stage 4). Stage 5, timeouts and the server's close, done (2026-10-29; see Learned in stage 5). Stage 6,
the client over HTTP/1.1, done (2026-10-29; see Learned in stage 6). The server's close reaching a WebSocket whose peer
never lets its reads wait, fixed (2026-10-29; see Fixed after stage 6). Stage 7, extended CONNECT in the session, done
(2026-10-29; see Learned in stage 7). Stage 8, WebSockets over HTTP/2 on the server, done (2026-10-29; see Learned in
stage 8). Stage 9, WebSockets over HTTP/2 in the client, done (2026-10-30; see Learned in stage 9).

## Goal

A handler can accept a WebSocket, and a program can open one, over HTTP/1.1 (RFC 6455) and over HTTP/2 (RFC 8441's
extended CONNECT), with TLS or without: for a side project's live page (a browser's `new WebSocket(...)`), a chat or
game server, or a client of someone else's streaming API. Conformant enough to pass the Autobahn TestSuite's core
cases (framing, fragmentation, UTF-8, control frames, the close handshake), bounded (message size, idle connections,
what's queued to send), covered by the server's limits and its graceful `close()`, and costing nothing to requests
that never upgrade. Hand-rolled, as the rest is: the frames are small, and SHA-1 (the one cryptographic step) is a
hundred lines.

## Where things stand

- **The server ignores `Upgrade`.** `http11::request_decode()` keeps it as any field; nothing reads it. A handler can't
  send 101: `response_writer::start()` throws `std::system_error` (`not_supported`) for `status::SWITCHING_PROTOCOLS`
  (`src/http/response.cpp`), as the HTTP/2 plan's stage 5 decided (its Decided question 2: "until something can switch
  protocols (extended CONNECT, or an Upgrade API)").
- **HTTP/1.1 connections are the server's.** `serve_connection()` holds the `detail::connection` (`opened`: a
  `tcp_socket` or `tls::stream`) and closes it when `serve_requests()` returns; `serve_requests()` holds the
  `connection_state` (a 16 KiB `buffered_writer`, an `io::flush_before_read` over the connection, a 16 KiB
  `buffered_reader` over that, the reused `server_request`/`server_response`, a 4 KiB scratch buffer, the stop source,
  `close()`'s and `abort()`'s callbacks, and the watch). A handler sees only `req.body` (a `limit_reader` or
  `chunked_reader` over `state.reader`) and its `response_writer` (whose HTTP/1.1 back end writes to `state.writer`).
  After the handler, the loop sends a default 200, `finish()`es, discards the body's rest and, per `end_request()`,
  waits for the next head (keep-alive). The socket has `body_read_timeout` (60 s) as its read timeout and
  `write_timeout` (60 s) as its write timeout; the head has `header_read_timeout`. The connection holds a
  `max_connections` slot until `serve_connection()` returns. `close()` shuts an idle connection's reading side and lets
  a busy one finish its request; `abort()` shuts the reading side whatever, and stops `req.stop`.
- **The watch reads the connection while a handler waits.** Once the handler has suspended and the request's body is
  at its end (at once for a GET), `begin_watch()` resumes the connection's watcher, which `fill()`s `state.reader`
  without a read timeout, `input` held; the server takes the watch's read over after the handler, or abandons it
  (`abandon_watch()`: a read deadline of now). A handler that read the connection itself while watched would collide
  with the watcher's read (`device_or_resource_busy`, one read operation per socket) and share its buffer.
- **HTTP/2 streams are the connection's.** `http2_server_connection` makes an `http2_stream` per request, runs its
  handler as a task (counted against `http2_max_concurrent_streams`), and ends the stream after it (`http2_finish()`).
  The request body is a `stream_reader` (WINDOW_UPDATEs as it's read), and `send(status)` gives a body writer framed
  as it goes, ended by `finish()`: a bidirectional byte stream already, but for the per-stream `read_timeout` and
  `write_timeout` (`body_read_timeout`, `write_timeout`: the timer resets a stream whose wait makes no progress), and
  `close()`'s drain, which waits for streams to end.
- **HTTP/2 refuses extended CONNECT.** The session's `pseudo_slot()` knows `:method`, `:scheme`, `:authority` and
  `:path`, so a `:protocol` makes the request malformed (a stream error), and `valid_request()` wants CONNECT with
  `:method` and `:authority` only. `http2::setting_id::enable_connect_protocol` (0x8) is declared in `http2.hpp`, but
  `settings` has no field for it and `apply_setting()` ignores it (its `default:`). `upgrade` is a connection-specific
  field (malformed in a received block, dropped by `append_fields()`), and a 101 is malformed over HTTP/2.
- **The client can't use a connection after a 101.** `send_http11` returns a 101 as the final response;
  `http11::framing_of()` frames its body as the rest of the connection (`response_framing::connection`), which
  `pooled_body` reads until it ends, and the connection isn't reused. Nothing can write to it: the request's body is an
  `io::reader` consumed while sending. The pool's TLS context offers ALPN `h2` and `http/1.1`, so a pooled TLS
  connection may agree on HTTP/2, where an upgrade has to be extended CONNECT.
- **What exists to build on.** `encoding::base64` (strict decoding). SHA-1 is in the trimmed Botan (`x509` requires
  it, `BOTAN_HAS_SHA1` in the installed `build.h`; the portfile lists `sha1_*` for the CPUs' versions), but not in a
  `NET_TLS=OFF` build, which has no Botan. An incremental UTF-8 check (`utf8_check`, a byte at a time, RFC 3629's
  rules) is in `src/http/structured_fields.cpp`'s anonymous namespace. The OS's randomness (`getrandom()`,
  `arc4random_buf()`, a batch per thread) is `dns::detail::random_id()`'s, in `src/dns/native.cpp`.
  `socket::set_stop_token()` cancels a waiting read having read nothing; `tls::stream::set_stop_token()` forwards to its
  socket, and a canceled TLS read isn't a stream failure. `tls::stream`'s flush queue (one writer at a time; a read's
  replies left to the writer in progress) is the model for interleaving control frames with a message being sent.

## Specifications and references

- [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html), The WebSocket Protocol: the opening handshake (4; the GUID
  `258EAFA5-E914-47DA-95CA-C5AB0DC85B11`, and 4.2.1's example key `dGhlIHNhbXBsZSBub25jZQ==` whose accept is
  `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`), framing (5.2; masking 5.3, client to server only; fragmentation 5.4, control frames
  interleaved, at most 125 bytes and never fragmented; 5.7's example frames), ping and pong (5.5.2/3: a pong may answer
  only the latest ping), the close handshake (5.5.1, 7: codes in 7.4, 1005/1006/1015 never on the wire; a reason in
  UTF-8; the server closes TCP first, 7.1.1), failing the connection (7.1.7), UTF-8 (8.1).
- [RFC 8441](https://www.rfc-editor.org/rfc/rfc8441.html), Bootstrapping WebSockets with HTTP/2:
  SETTINGS_ENABLE_CONNECT_PROTOCOL (0x8, 0 or 1, never withdrawn; "Receipt of this parameter by a server does not have
  any impact"); a client may use extended CONNECT once it has received 1; `:method` CONNECT with `:protocol`
  (`websocket`), `:scheme`, `:path` and `:authority` (not a tunnel's target); no `Upgrade`, `Connection` or
  `Sec-WebSocket-Key`/`-Accept`, but `Sec-WebSocket-Version`, `-Protocol`, `-Extensions` and `Origin` as in RFC 6455;
  DATA carries the frames; END_STREAM is TCP's orderly close, RST_STREAM(CANCEL) its reset. Read for this plan.
- [RFC 7692](https://www.rfc-editor.org/rfc/rfc7692.html), permessage-deflate (RSV1, DEFLATE per message, context
  takeover and window bits negotiated); [RFC 1951](https://www.rfc-editor.org/rfc/rfc1951.html), DEFLATE.
- [RFC 9220](https://www.rfc-editor.org/rfc/rfc9220.html), WebSockets over HTTP/3: parked with `http3.md`.
- [RFC 3174](https://www.rfc-editor.org/rfc/rfc3174.html) and FIPS 180-4 (SHA-1, and their test vectors); RFC 3629
  (UTF-8); RFC 9110 7.8 (`Upgrade`), 15.2.2 (101), 15.5.22 (426 Upgrade Required).
- WHATWG's [WebSockets standard](https://websockets.spec.whatwg.org/) (browsers' client: whole messages,
  `bufferedAmount`, scripts close with 1000 or 3000-4999, and a handshake response that isn't 101 fails the connection:
  no redirects).
- The [Autobahn TestSuite](https://github.com/crossbario/autobahn-testsuite) (Apache 2.0): `fuzzingclient` tests a
  server, `fuzzingserver` a client; sections 1-7 (framing, pings, reserved bits, opcodes, fragmentation, UTF-8, close),
  9 (performance), 12-13 (permessage-deflate). Distributed as a Docker image (`crossbario/autobahn-testsuite`); its
  PyPI package is Python 2 only (from memory).

## How others do it

- **Go, `golang.org/x/net/websocket`**: the old package (a `Handler` and `Conn` as an `io.ReadWriter`, a `Codec` for
  messages); frozen, and its docs point to the two below.
- **Go, gorilla/websocket** ([docs](https://pkg.go.dev/github.com/gorilla/websocket)): `Upgrader{CheckOrigin,
  Subprotocols, ReadBufferSize, WriteBufferPool, EnableCompression}.Upgrade(w, r, header)` hijacks the connection
  (`http.Hijacker`) and returns a `*Conn`, replying with an HTTP error itself if the handshake fails. CheckOrigin's
  default refuses a present `Origin` whose host isn't `Host`. Messages: `NextReader()` (one open reader; the next call
  discards what's left), `NextWriter(type)`, and `ReadMessage`/`WriteMessage` built on them. "Connections support one
  concurrent reader and one concurrent writer"; `Close` and `WriteControl` may be called concurrently with anything.
  `SetReadLimit` (no default), read and write deadlines, ping/pong/close handlers (by default a ping is answered and a
  close echoed); "The application must read the connection to process close, ping and pong messages". Closes come back
  as `*CloseError{Code, Text}`. Compression: experimental, no context takeover only. Over HTTP/1.1 only.
- **Go, coder/websocket** (formerly nhooyr; [docs](https://pkg.go.dev/github.com/coder/websocket)):
  `Accept(w, r, *AcceptOptions{Subprotocols, OriginPatterns, InsecureSkipVerify, CompressionMode})` and `Dial(ctx, url,
  *DialOptions)` (through an `http.Client`, returning the `*http.Response` too); `Reader(ctx)`/`Writer(ctx, type)` (one
  of each open at a time; a second `Writer` waits) and `Read`/`Write`; "All methods may be called concurrently except
  for Reader and Read"; a default read limit of 32768 bytes (closing with 1009 past it); `Close(code, reason)` writes
  the close frame and waits up to 5 s for the peer's, `CloseNow()` doesn't; `CloseRead(ctx)` reads in the background,
  failing the connection (1008) on a data message; `Ping(ctx)` waits for the pong, so needs a reader running.
  `CloseStatus(err)` gives the code. Origin: same host unless `OriginPatterns` match. Compression off by default ("Do
  not enable compression without benchmarking"). HTTP/2: not supported, on its roadmap.
- **Go's own HTTP/2** supports extended CONNECT from 1.24 (x/net's branch vendored into it), behind
  `GODEBUG=http2xconnect=1`: off by default because "Browsers interpret a server advertising extended CONNECT support
  as indicating the server supports WebSockets-over-HTTP/2", which Go's WebSocket packages don't, so advertising it
  "is a non-backwards-compatible change" (x/net commit 68c5fdd, golang/go#71128 and #49918). The client asks for it
  with a `:protocol` header on the request.
- **Rust, tungstenite / tokio-tungstenite**: a sans-I/O-ish `WebSocket<Stream>` (`read()`/`send()` of whole `Message`s,
  `Text`/`Binary`/`Ping`/`Pong`/`Close`/`Frame`); tokio-tungstenite's `WebSocketStream` is a `Stream` and `Sink`,
  `split()` into a reading and a writing half (from memory). `WebSocketConfig` (0.30): `max_message_size` 64 MiB,
  `max_frame_size` 16 MiB, read and write buffers 128 KiB, `max_write_buffer_size` unlimited,
  `accept_unmasked_frames` false. Pings are answered by queuing a pong, flushed with the next write or `flush()`.
- **Rust, hyper**: the handshake's HTTP part is the application's (or a crate's): `hyper::upgrade::on(&mut req)` is a
  future that resolves to `Upgraded` (the connection, with what was read past the head) once the 101 is sent, which
  the application spawns a task to drive; the server's connection must be built `.with_upgrades()`, and an upgraded
  connection is no longer the server's. HTTP/2: `http2::Builder::enable_connect_protocol()`, and the request's
  `hyper::ext::Protocol` extension (from memory).
- **Boost.Beast's `websocket::stream<NextLayer>`**, the closest C++ analogue: `accept()`/`handshake()` (and decorators
  for the handshake's fields), `read(buffer)` of a whole message or `read_some`, `write`/`write_some` (`auto_fragment`,
  `write_buffer_bytes`), `text()`/`binary()`, `control_callback` for pings and pongs, `read_message_max` (16 MB by
  default), `close(reason)`. One read and one write outstanding at a time, with `async_ping`/`async_close` allowed
  beside a write (from memory). Timeouts (`stream_base::timeout`, off by default): `handshake_timeout`, `idle_timeout`
  ("If no data or control frames are received from the peer for a time equal to the idle timeout") and
  `keep_alive_pings`: "a ping frame will be sent to the peer if no data is received for half of the idle timeout
  interval". `timeout::suggested(role_type::server)` is 30 s for handshakes, 300 s idle, with pings (from memory).
  permessage-deflate through zlib, optional.
- **Browsers** answer pings, never send them, offer `permessage-deflate; client_max_window_bits`, and fail a handshake
  answered with anything but 101 (or, over HTTP/2, 2xx). Firefox has used RFC 8441 since version 65, Chrome since
  around version 91, when the server's SETTINGS has ENABLE_CONNECT_PROTOCOL (from memory): with no fallback to
  HTTP/1.1 if the extended CONNECT is refused, which is why Go turned the setting off.

## Design

### Where it lives

A module of its own, `websocket/` (`net::websocket`, `include/websocket/`, `src/websocket/`, `test/websocket/`),
depending on `http/` (the handshake is HTTP's), as `instrument/` does (Decided question 1). Public:
`websocket/websocket.hpp` (`connection`, `accept()`, `connect()`, the options) and `websocket/error.hpp`. Internal, as
HPACK's are: `websocket/frame.hpp` (the codec), `websocket/session.hpp` (the protocol without I/O),
`websocket/handshake.hpp`. HTTP gets one internal header, `http/tunnel.hpp` (below), and knows nothing else of
WebSockets but RFC 8441's `:protocol`. Three helpers go to `util/` (which depends on nothing but `encoding/`):
`util/sha1.hpp` (Decided question 2), `util/utf8.hpp` (`util::utf8_validator`: `structured_fields.cpp`'s check moved,
with a `feed(std::span<const std::byte>)` that skips ASCII 8 bytes at a time, and `complete()`; Structured Fields' use
it) and `util/random.hpp` (`util::random_bytes(std::span<std::byte>)`: `dns/native.cpp`'s `fill_random()` and its
per-thread batch moved, `random_id()` built on it).

### The codec and the session (sans-I/O)

`websocket/frame.hpp`: `enum class opcode` (continuation 0, text 1, binary 2, close 8, ping 9, pong 10);
`frame_header{fin, rsv (3 bits), op, masked, mask_key (std::array<std::byte, 4>), length (std::uint64_t)}`;
`decode_header(std::span<const std::byte>)` returning the header and its size, `std::nullopt` if more bytes are needed
(2 to 14), or a close code for what's malformed on its own (a 64-bit length with its top bit set; a length not in
its minimal form, which RFC 6455 5.2 says MUST be, refused as the HTTP/1.1 decoder refuses what's ambiguous);
`encode_header(header, out)` (at most 14 bytes, into a `std::array`); `apply_mask(std::span<std::byte>, key, offset)`,
XOR in place a word at a time, `offset` the position in the payload so a payload can be unmasked in pieces; and the
close payload's `parse_close()`/`encode_close()` (a code and a reason, or none: a payload of 1 byte, a code that can't
be on the wire, or a reason that isn't UTF-8 is a protocol error; the codes allowed are 1000-1003, 1007-1014 and
3000-4999: below 1000, 1004-1006 and 1015 (never sent) and 1016-2999 (unassigned) aren't).

`websocket::detail::session` (`websocket/session.hpp`) is the protocol's rules, as `http2::session` is HTTP/2's, but
pull-shaped rather than event-shaped, so a payload is never copied: its owner reads bytes into its own buffer (or the
caller's) and hands them over.

- `session(role, limits)`, `role::server` requiring masked frames and `role::client` refusing them.
- `begin_frame(const frame_header&)` checks the frame against the state: RSV bits set without an extension that
  allows them (none, until Decided question 15); an unknown opcode; a control frame fragmented or over 125 bytes; a
  continuation with no message started, or a text/binary frame inside one; masking per role; the message's total past
  `limits::max_message`; anything but a close after the peer's close. It returns what the frame is (data, of which
  message type and whether it's the message's first and last; or a control frame) or the close code to fail with
  (1002, 1009).
- `payload(std::span<std::byte>)` unmasks bytes of the current frame in place, feeds a text message's to its
  `utf8_validator` (whose state carries across reads and frames; a character split across fragments is fine, an
  invalid byte fails at once, 1007, and an incomplete character at the message's end fails then), and for a control
  frame collects them into the session's own 125 bytes. `end_frame()` finishes a control frame, returning a ping (to
  answer), a pong, or a close (its code and reason, or 1005 for none), and moves the close state.
- Sending: `frame_for(opcode, fin, length)` gives a header (a client's with a fresh mask key from `util::random_bytes`,
  taken from a per-thread batch), and checks the sending side's state (one message at a time, nothing after a close
  sent, control payloads at most 125 bytes).
- The close state: open, close sent (only a close may be received; data received is dropped, as coder's `Close` does),
  close received (our close is the reply, echoing the code, as browsers and gorilla do), closed.

`fuzz_websocket` drives a server's and a client's session with the input as frames, whole and a byte at a time, and
checks they agree (the same messages, the same failure at the same byte), that what's accepted re-encodes to frames that
decode the same, and `utf8_validator` against a plain decoder of RFC 3629.

### The handshake

`websocket/handshake.hpp`, plain functions over `server_request`/`headers`: `accept_key(key)` (SHA-1 of the key and
the GUID, in base64); `check_request(const server_request&)` (HTTP/1.1: GET, version 1.1 or later, `Upgrade` with the
token `websocket` and `Connection` with `upgrade`, ignoring case, `Sec-WebSocket-Version` 13, `Sec-WebSocket-Key`
whose strict base64 decodes to 16 bytes, no body; HTTP/2: the extended CONNECT below, version 13, no key);
`select_subprotocol(server's list, request)` (the first of the server's that the client offers, gorilla's and coder's
rule; `Sec-WebSocket-Protocol` may be several lines); `check_origin(req, options)` (Decided question 7). Extensions
offered are ignored: none is accepted, so `Sec-WebSocket-Extensions` isn't sent and RSV bits stay invalid. The client's
side: `make_request()` (a random 16-byte key, `Origin` only if given) and `check_response()` (101, `Upgrade`,
`Connection`, the accept, a subprotocol it offered, no extension it didn't).

### The server: switching an HTTP/1.1 connection

`websocket::accept(server_request& req, response_writer& rw, const accept_options& options = {})` returns a task of
`std::expected<connection, std::error_code>`. It checks the request; on failure it responds itself (400, 403 for an
origin, or 426 with `Sec-WebSocket-Version: 13`, as RFC 6455 4.2.2 asks) and returns why (`websocket::errc`). Otherwise
it switches: `http::detail::switch_protocols(rw, fields)` (internal, `http/tunnel.hpp`) sends 101 with `Upgrade:
websocket`, `Connection: Upgrade`, `Sec-WebSocket-Accept`, the subprotocol, and what the handler put in `rw.headers()`
(e.g. `Set-Cookie`), flushes it, and returns a `tunnel`. `send(101)` itself still throws: switching is only through
this, so a 101 always comes with a connection that has really switched.

`http::detail::tunnel` is the bytes after the switch, as an abstract class with three implementations (the server's
HTTP/1.1 connection, an HTTP/2 stream, the client's connection): an `io::buffered_reader&` and an
`io::buffered_writer&`, `set_read_timeout()`, `set_stop_token()` (cancels a waiting read, having read nothing),
`closing()` (the server's `close()`'s token, none for a client), and `abort()` (an abrupt end: the HTTP/1.1 socket's
reading side shut, an HTTP/2 stream reset with CANCEL).

How `response_writer` reaches the connection without the server growing per request: `connection_state` holds a
`detail::switch_hook` (a function pointer and the state, as `io::start_callback` is), made once per connection, and
the HTTP/1.1 back end of every `response_writer` gets its address (one pointer more in `http11_backend`, which is the
variant's larger alternative already). The hook, on the handler's task:

- closes the watch's gate for the request: a third mark, `switched`, `fetch_or`ed into `watch_gate`, after which
  `gate_open()` never begins a watch; if the gate had opened, a watch was begun, which it abandons as the server's paths
  do (a read deadline of now, `watch_ended`, `after_watch()`), keeping what that read buffered;
- holds `input` (`flush_before_read::hold(true)`), so the tunnel's reads never flush the writer, which another task
  may be writing to;
- clears the socket's read timeout (`body_read_timeout`; the tunnel's `set_read_timeout()` sets the WebSocket's), and
  keeps `write_timeout`;
- encodes the 101 into `state.writer` and flushes, then marks the response `switched()` (and `sent()`).

`state.reader` and `state.writer` are the tunnel's buffers: what the client sent after its request (it shouldn't, but
may) is already in the reader, and nothing more is allocated for buffers. After the handler returns, the loop tests
`rw.switched()` once: no default response, `finish()`, discarding or keep-alive; it flushes and ends the connection,
which `serve_connection()` closes (close_notify first over TLS; the server closing TCP first, as RFC 6455 7.1.1 has it).
The WebSocket's life is the handler's call (Decided question 3): the connection keeps its `max_connections` slot,
`join()` waits for it, and `abort()`'s callback (shutting the reading side, stopping `req.stop`) ends it, all unchanged.

### The server: HTTP/2 streams (RFC 8441)

The session (`http2_session.cpp`) learns `:protocol`: `pseudo_slot()` takes it for a server, `valid_request()` accepts
CONNECT with `:protocol`, `:scheme`, `:path` and `:authority` if this side has sent ENABLE_CONNECT_PROTOCOL = 1 (a
`:protocol` otherwise, or on any other method, is malformed, as before), and `settings` gets `enable_connect_protocol`,
sent by a server whose `session_config` says so and read by a client (0 or 1; 1 then 0 is PROTOCOL_ERROR, as RFC 8441
3 forbids withdrawing it; ignored from a client). `server_config::http2_websockets` (Decided question 12) sets it.

The server's `start_stream()` sets `server_request::protocol` (a `std::string`, empty unless the request was an
extended CONNECT) and presents a `websocket` one as Decided question 11 decided (as GET with `protocol`
set, so one route serves both versions); any other `:protocol` gets 501, as an unknown method does. `accept()`
answers it with 200 (no `Sec-WebSocket-Accept`; the subprotocol), through a new entry of `http2_stream_ops`
(`open_tunnel`, in the static table), which sends the head without END_STREAM, turns the stream's `read_timeout` off
(the WebSocket's own idle timeout replaces it), and returns a tunnel over the stream's `stream_reader` and
`stream_writer`, with a `buffered_reader` and a `buffered_writer` of 4 KiB each over them (frame headers and small
messages gathered, large payloads written through; a flush submits to the stream, whose DATA the connection's scheduler
frames). `set_stop_token()` registers a callback that sets a
flag on the stream and notifies its `changed` (taking `mu`, never under it), which `stream_reader`'s wait checks, so
a read can be interrupted with nothing read, as a socket's. When the handler returns, `http2_finish()` ends the stream
(END_STREAM, RFC 8441's orderly close); `abort()` resets it (CANCEL). A stream that becomes a WebSocket still counts
against `http2_max_concurrent_streams`, so a connection's WebSockets are bounded as its requests are.

### The connection: messages, control frames, the close

`websocket::connection` (movable; its state, the session and the tunnel, is one allocation per WebSocket, so that
the address the write queue's waiters hold doesn't move):

- **Reading** (Decided question 4): `read_message(std::string&)` / `read_message(std::vector<std::byte>&)` replace the
  buffer's contents with the next whole message (no allocation once the buffer's capacity suffices), returning its
  `message_type` (text or binary); and streaming, `next_message()` returning the type, then `message()` as an
  `io::reader` of it, ending with `end_of_stream` at the message's end (unread rest skipped by the next
  `next_message()`, as gorilla's). A read takes bytes from the buffered reader, or (past a frame header, with nothing
  buffered) straight into the caller's span, then hands them to `payload()` to unmask and validate in place. Control
  frames between fragments are handled inside the read: a ping is answered (one pending pong, the latest, as RFC 6455
  5.5.3 allows, so a flood of pings costs 125 bytes), a pong noted, a close answered and reported. One read at a time:
  a second concurrent one fails with `device_or_resource_busy`, as a socket's.
- **Writing**: `write_message(message_type, std::span<const std::byte>)` (one frame), `begin_message(type)` returning
  an `io::writer` (each `write()` a frame, `end_message()` a final empty continuation) and `ping(payload)`. Writes from
  several tasks are serialized (Decided question 5): the write side is a turn (a `coro::notifier`, as `tls::stream`'s
  flush queue), held for a whole message, so messages never interleave; control frames go between a streamed message's
  frames. A read's replies (pong, the close's echo) don't wait for a writer: if a write holds the turn, they're left to
  it (it sends them after its frame); otherwise the read writes them itself. The server writes the payload through the
  buffered writer (large ones through `write_through`); the client masks it into `prepare()`d space, a buffer's worth at
  a time. Each message (or streamed frame) is flushed before its write returns, so a write's completion is the
  backpressure: nothing queues beyond the buffered writer's 16 KiB and the pending control frames, and a peer that
  doesn't read makes writes wait, bounded by the transport's write timeout (the server's `write_timeout`, the client's
  `io_timeout`). Reading is the peer's backpressure in turn: TCP's window, or HTTP/2's stream window given back only as
  read.
- **Closing**: `close(close_code, reason)` sends a close (once), then waits for the peer's, up to
  `options::close_timeout` (5 s, coder's), reading and dropping data itself if no read is in progress, or waiting
  (on a notifier) for the reading task to see it; the client then waits for the server's end of the stream, as RFC 6455
  7.1.1 asks, within the same timeout. `connection` going out of scope without a close aborts the tunnel (no close
  frame: the peer sees 1006). A close that fails, or times out, aborts.
- **How the end is reported** (Decided question 10): errors are `std::error_code`s, as everywhere. Close codes are in
  `websocket::close_category()`, which `close_code` (`normal_closure` 1000, `going_away`, `protocol_error`,
  `unsupported_data`, `no_status_received` 1005, `abnormal_closure` 1006, `invalid_payload` 1007, `policy_violation`,
  `message_too_big` 1009, `mandatory_extension`, `internal_error`, and 3000-4999 as numbers) makes codes in: the peer's
  close comes back from the read as its code (1005 for none), and `close_reason()` is its text; a close this side sent
  for a failure it found (1002, 1007, 1009) at `sent_base` plus the code, as `tls::category()` keeps alerts sent apart,
  with `websocket::from_peer(err)`; the stream ending without a close is 1006. `normal_closure`, `going_away`,
  `no_status_received` and `abnormal_closure` are equivalent to `io::status_condition::closed`, so code that only asks
  "did it end" needn't know WebSockets. Handshake failures are `websocket::errc` (`not_websocket`, `bad_version`,
  `bad_key`, `origin_refused`, `bad_status`, `bad_accept`, `bad_subprotocol`, `bad_extension`) in
  `websocket::category()`. Every operation after a failure returns it again (sticky), as gorilla's read errors are.

### Limits, timeouts and cancellation

`connection_options` (inside `accept_options` and `connect_options`):

- `max_message` (Decided question 6: 1 MiB) bounds a message, whichever API reads it (streaming too, so a
  message is bounded as a body is); a frame is bounded only by it. Past it, 1009 and the connection fails.
- `idle_timeout` (Decided question 8: 60 s) with keep-alive pings as Beast's: while a read waits, its
  transport's read timeout is half of it; when that passes with nothing received, the read sends a ping (through the
  write turn, as a reply) and waits the other half; nothing by then fails the connection with `timed_out` (aborting it).
  So pings cost no task and no timer, and the transport's existing timeout does the timing; but only while the
  application reads, which it must anyway (as gorilla's and coder's docs say) for pings and closes to be answered.
  0 turns it off.
- `close_timeout` (5 s) and, for the client, the handshake's timeouts (`connect_timeout`, `response_header_timeout`).
- `stop` (a `std::stop_token`; the server's default is `req.stop`): stopping it fails what's waiting
  (`operation_canceled`) and aborts the tunnel (the client-leaving case of HTTP/1.1's `pooled_body`). `abort()` reaches
  a server's WebSockets through it as well as through the socket. `http::timeout` on a WebSocket route therefore bounds
  the whole session (its linked token is `req.stop` by then), which the guide says.
- **The server's `close()`** (Decided question 9: going away): the tunnel's `closing()` token, through
  `set_stop_token()` on the transport (the socket, linked by a `util::linked_stop_source` when there's also `stop`; the
  HTTP/2 stream's flag), interrupts a waiting read with nothing read; the read sends a close with 1001 (`going_away`),
  clears the token, and reads on for the peer's close within `close_timeout`, then reports `going_away` (sent). Without
  a read in progress, nothing is noticed until the next one (documented, as pings are). HTTP/2's drain (GOAWAY) then
  finds the stream ended.

### Middleware and routing

A WebSocket endpoint is a route like any other: `GET("/chat"_route, chat)` in either router, the handler calling
`accept()`; routing, path values and route middleware run before it, as for any request (authentication middleware
can answer 401 before the handshake). Over HTTP/2 the same route serves extended CONNECT as Decided question 11 went
(as GET). `instrument::http_metrics` and `http_tracing` see the switch as the response's status (101, or 200 over
HTTP/2) and the whole session as the request's duration (the guide says so; a histogram of WebSocket routes'
durations means sessions'). `server_request::extensions` and `route` stay valid for the handler's call, which is the
WebSocket's life.

### The client

`websocket::connect(http::client& client, std::string_view url, const connect_options& options = {})` (Open question
13) returns a task of `std::expected<connection, std::error_code>`: `ws://` and `wss://` (and `http(s)://`, as browsers
now accept, from memory). Until RFC 8441's client stage it makes its own connection, apart from the pool (not counted in
`max_connections_per_host`, never returned to it), with the client's configuration: its scheduler, resolver,
`connect_timeout`, `attempt_delay`, `tls` (a `tls::client_context` of the connector's own, made on first use from the
same `tls::client_options`, offering only `http/1.1` with ALPN, so a server can't choose HTTP/2), and
`response_header_timeout` for the handshake. It sends the request with `http11::request_encode_head()` and decodes the
response with `http11::response_decode()` on its own `buffered_reader` (16 KiB, as the server's), whose leftovers are
the first frames; `connect_options::headers` (e.g. `Authorization`, a cookie), `subprotocols`, `origin`. Redirects
aren't followed (Decided question 14). `connect_options::stop` covers connecting and the handshake (and, kept, the
connection). A response other than 101 fails with `bad_status`; a 101 whose accept, subprotocol or extensions are
wrong fails too (RFC 6455 4.1: the client MUST fail the connection).

Over HTTP/2 (stage 9; decided by the user, 2026-10-30): when the client's pool has an HTTP/2 connection to the
WebSocket's host whose peer's SETTINGS have arrived saying ENABLE_CONNECT_PROTOCOL = 1, with a stream free, and
`connect_options::http2` is on (**on by default**, as browsers have it; it applies only where the server has advertised
the setting, which cppnet's server does by default), `connect()` opens an extended CONNECT stream on it instead
(`http2_client_connection::open_tunnel(request)`, internal: the request head without END_STREAM, `:protocol`, the
stream's `read_timeout` off, its reader and `stream_writer` as the tunnel, a reset on `abort()`). **Without a usable
HTTP/2 connection, HTTP/1.1 on a connection of its own**, as above (Chrome's rule): the client never connects through
the pool to find out, and RFC 8441 forbids extended CONNECT before the setting arrives, so a connection still waiting
for the server's first SETTINGS isn't used. (Measured in stage 8: Chrome 156 uses HTTP/2 only on a connection it has
already; Firefox 143 connects offering h2.) **Only the WebSocket host's own connections**: never a coalesced one, even
with `client_options::http2_coalescing` on, so there's no 421 path.

## Costs

- **Requests that don't upgrade, HTTP/1.1**: the decoder is unchanged (`Upgrade` is still just a field); each
  `response_writer` gets one pointer more (the connection's switch hook, set once per connection); after each handler,
  one test of `switched()`. Nothing allocated, no coroutine added. Measured with `bench_request` (time and allocations
  per request, before and after) in stage 3.
- **HTTP/2**: `pseudo_slot()`'s comparison for `:protocol` comes after the four others, so it runs only for a
  pseudo-header that's none of them (a malformed request); `scanned_fields` grows by a `std::optional<std::string_view>`
  (on the stack); `settings` by a `bool`; `server_request` by a `std::string` (`protocol`, empty: no allocation),
  cleared by `clear()`; `http2_stream_ops` by a function pointer (one static table). `bench_request`'s HTTP/2 tests in
  stages 7 and 8.
- **Per WebSocket on the server**: over HTTP/1.1, what the connection holds already (its two 16 KiB buffers, 4 KiB
  scratch, the request and response, the watcher's frame), the handler's frame, and the connection's state (one
  allocation: the session with its 125-byte control buffer, a pending pong's 125 bytes, the close reason, the write
  turn's notifier and the tunnel: under 1 KiB). Over HTTP/2, the stream (as any request's), the state, and the tunnel's
  two 4 KiB buffers. The client: the connection, two 16 KiB buffers, and the state; over HTTP/2 (stage 9), a stream of
  the client's connection (as a request's), its tunnel's two 4 KiB buffers, the transport that holds it (one
  allocation), and the state.
- **Per message**: nothing allocated by `read_message()` into a buffer with room, nor by writes; masking and unmasking
  are a pass of XOR a word at a time (the client's a copy, into the writer's buffer), UTF-8 validation a pass over a
  text message's bytes (ASCII 8 at a time). `bench_websocket` (stage 6) measures messages a second and allocations a
  message over loopback, small and large, cppnet's client against cppnet's server.

## Testing

- **On bytes** (`test/websocket/frame.cpp`, `session.cpp`, `handshake.cpp`): RFC 6455 5.7's example frames (both
  directions, masked and not, fragmented, ping and pong, 256 bytes and 64 KiB lengths), 4.2.1's key and accept; the
  session's rules table-driven (`GENERATE(test_case{...})`), covering what Autobahn's sections 1-7 test, written from
  its case descriptions rather than copied: each length form, reserved bits, reserved opcodes, control frames too long
  or fragmented, pings between fragments, continuations out of place, close payloads (1 byte, each code allowed and
  refused, reasons valid and not, 123 bytes), UTF-8 (Markus Kuhn's test file's sequences as Autobahn's section 6 uses
  them: valid, overlong, surrogates, past U+10FFFF, truncated, split across frames at each byte, invalid failing at the
  byte); the handshake's checks (each missing or wrong field, several `Sec-WebSocket-Protocol` lines, the origin rules).
  `util::sha1` against RFC 3174's and FIPS 180-4's vectors (and lengths around 55, 56 and 64 bytes);
  `util::utf8_validator` and Structured Fields' tests unchanged.
- **Fuzzing**: `fuzz_websocket`, as above, a 10-second ctest run like the others.
- **Loopback, server** (`test/websocket/server.cpp`, `[websocket][server]`): a scripted raw client
  (`test/websocket/raw_peer.hpp`: sends a handshake and frames by hand, masked or not, a byte at a time or in bursts,
  and records what comes back, frame by frame) against a handler that echoes: messages of each type and size,
  fragmented, with pings between fragments; the 101's fields; refusals (400, 403, 426) and that the connection then
  serves the next request; unmasked frames (1002), invalid UTF-8 (1007), a message past the limit (1009); the close
  handshake both ways, and the server closing TCP after it; frames sent with the request (before the 101) kept; a
  handler that suspended before `accept()` (so the watch had begun) and one that didn't; idle pings at half the timeout
  and the failure after it (short timeouts); `close()` sending 1001 to a WebSocket that's reading, and `join()`
  returning after; `abort()`; `max_connections` counting WebSockets; several tasks writing at once (messages whole,
  never interleaved); a peer that doesn't read (writes wait, then fail at `write_timeout`); over TLS
  (`test/tls/server.cpp`, with `test_pki.hpp`).
- **Loopback, client** (`test/websocket/client.cpp`): cppnet's client against cppnet's server (echo, subprotocols, close
  from either side, cancellation by `stop` during the handshake and during a read), and against a scripted raw server
  (bad accept, a 200, a 302, a subprotocol not offered, an extension not offered, frames right after the 101, a server
  that never closes TCP), and that the client's frames are masked with keys that change.
- **HTTP/2**: the session's extended CONNECT on bytes (`test/http/http2.cpp`, `[session][connect]`: the setting sent,
  received, withdrawn, `:protocol` with and without it, missing `:path`); `fuzz_http2` gets `:protocol` and the setting
  in its scripts; the server with `test::h2_client` (which learns to send an extended CONNECT and carry raw bytes in
  DATA), `[http2][server][websocket]`: the 200, echo, END_STREAM at the close, a reset aborting the handler's WebSocket,
  no read timeout on the stream, 501 for another `:protocol`, the route serving both versions; and cppnet's client
  against cppnet's server over prior knowledge and TLS.
- **Autobahn** (Decided question 16): `fuzzingclient` against `example/websocket-echo` and `fuzzingserver` against a
  small client driver (`example/websocket-autobahn`, which runs each case and echoes), by hand where Docker is
  available, sections 1-7 and 9 expected to pass (12-13 excluded with compression), its report kept out of the
  repository; and an optional workflow that does it on GitHub's Linux runner. Not part of the required `build`.
- **By hand**: `websocat ws://127.0.0.1:8080/echo` (and `wss://` with `--cert/--key`), a browser's console
  (`new WebSocket(...)`, Firefox and Chrome, which use HTTP/2 over TLS once the server advertises it), Python's
  `websockets` client, and `test/test_server.go` gaining an echo endpoint (gorilla or coder, in that throwaway module)
  for the client.

## Learned in stage 1

- `util::sha1` (`util/sha1.hpp`) is a class fed incrementally: `update()` (bytes, or a `std::string_view`) returns the
  hasher, so the handshake's accept key is `sha1{}.update(key).update(guid).finish()` without joining the strings;
  `finish()` pads, returns the 20 bytes (`sha1::digest`) and leaves the hasher as a new one. No one-shot function: the
  chain is one. The message schedule is kept to 16 words, computed as the rounds use them. Its tests are FIPS 180's and
  RFC 3174's vectors (the empty string, "abc", the 448-bit and 896-bit messages, a million 'a's, RFC 3174's fourth),
  lengths 55 to 65, 119, 120 and 128 (digests from Python's `hashlib`), every split of a 241-byte message in two and
  in pieces of 1 to 129 bytes, and RFC 6455 4.2.2's accept key, through `encoding::base64`.
- `util::utf8_validator` (`util/utf8.hpp`) keeps Structured Fields' rules (RFC 3629 as byte ranges: the second byte's
  range narrowed after E0, ED, F0 and F4; C0, C1, F5 to FF and continuation bytes refused as leads), so it fails at the
  first byte that no UTF-8 can have where it is: E0 80 fails at the 80, ED A0 (a surrogate) and F4 90 (past U+10FFFF) at
  their second bytes, not when the character's complete, which is what failing fast on a text frame's byte needs. Beside
  `feed(std::span<const std::byte>)` (out of line, `src/util/utf8.cpp`) it has an inline `feed(std::byte)`, which
  Structured Fields' Display Strings use for their percent-decoded bytes, and `reset()`. Once a feed has failed, its
  state is meaningless until `reset()`. The ASCII path is taken only at a character's boundary and on an ASCII byte:
  then whole words (8 bytes, loaded with `memcpy`, no high bit in any) are skipped, and the run's rest byte by byte, so
  text that's mostly multi-byte doesn't pay a word's load per character.
- The validator is tested against a decoder of code points rather than byte ranges (`test/util/utf8_reference.hpp`: a
  byte fails once its character so far can't be completed to a code point in range for its length, not a surrogate,
  not past U+10FFFF), over every sequence of one and two bytes and those of three and four from each lead byte with
  continuations at the ranges' edges, fed a byte at a time (failing at the same byte) and at once. `fuzz_utf8` checks
  the same on any input, and fed in pieces of several sizes (the piece that fails holds the byte that does): the plan
  had this inside `fuzz_websocket`, but it costs nothing on its own, before the codec exists.
- `util::random_bytes` (`util/random.hpp`) takes spans smaller than a batch (256 bytes, per thread; the DNS resolver's
  was 64 IDs, 128 bytes) from its end, refilling it part way through a draw if need be, and fills larger ones
  directly. `dns::detail::random_id()` is two of its bytes. `util/random.cpp` includes `config.hpp` for the platform
  macros (which depends on nothing). `util::uuid`'s own `fill_random()` (`std::random_device`) and OpenTelemetry's ids
  (`std::mt19937_64`) are left as they were.

## Learned in stage 2

- Everything internal is in `websocket::detail` (`frame.hpp`, `session.hpp`, `handshake.hpp`), as the plan named the
  session. `websocket/websocket.hpp` exists already, holding only the handshake's options (`accept_options{subprotocols,
  origins, insecure_any_origin}`, `connect_options{headers, subprotocols, origin}`); `connection`, `accept()`,
  `connect()` and `connection_options` come with stages 4 and 6. The session's limit is its own `detail::limits`
  (`max_message`, 1 MiB), which `connection_options` will fill.
- Errors (`error.hpp`): `close_code` is an error code enum, the peer's codes (`err == close_code::going_away` is the
  peer's going away), and `sent(code)` makes this side's, at `sent_base` 10000 (TLS's 512 fits alerts' 8 bits; 10000
  keeps a close's 16, and 11002 reads as 1002 sent). There's no condition enum matching either side's, as TLS's `alert`
  is (an enum can't be both a code and a condition): `close_code_of(err)` gives the code whichever side sent it, and
  `from_peer(err)` the side. 1000, 1001, 1005 and 1006 are `io::status_condition::closed` from either side. 1012 to 1014
  (IANA's service restart, try again later, bad gateway) are named, as the plan allows them on the wire.
- The codec's failures are the error the connection ends with, the close this side sends (`sent(protocol_error)`), not
  a bare close code, so they're `std::error_code`s as everything else's; the sender's own mistakes (`encode_close()`
  given a code that can't be sent, `frame_for()` misused) are `invalid_argument`. `decode_header()` returns
  `std::expected<std::optional<decoded_header>, std::error_code>` (none: more bytes), and fails a length as soon as its
  bytes are in, before the mask key, so a header fails at the same byte however it's read. `encode_close()` takes
  `no_status_received` as "a close without a code" (an empty payload), which a close needs and the plan didn't say how
  to ask for.
- The session's shape, where the plan left it open: `begin_frame()` gives a `frame_info` (data, with its message's type
  on every fragment and whether it's the first and last; control; or dropped), `end_frame()` a `received` (data,
  message_end, ping, pong, close with its code and reason, dropped), `remaining()` what's left of the frame's payload.
  Misuse (a frame begun inside another, more payload than the frame has, the end before the payload's) is
  `invalid_argument` and not kept; a protocol failure is kept, and every later receiving call returns it.
- After the peer's close every frame fails with 1002, a second close too: the plan had "anything but a close", but a
  second close is no better than data (RFC 6455 has the peer send nothing after its close), and the owner stops reading
  once it has the close. After this side's close, data, pings and pongs are dropped (pings not answered, as a pong
  would follow the close), but still checked for what the framing requires (RSV, opcodes, masks, control frames,
  continuations in order): only UTF-8 and `max_message` are skipped, as nothing's kept of them. A message begun before
  the close is still followed through its fragments, so its continuations aren't taken for strays.
- Sending: `frame_for()` refuses everything after this side's close with `broken_pipe` (writing after a shutdown,
  which `status_condition::closed` takes in), and after a receiving failure everything but the close that fails the
  connection. Data may still be sent between the peer's close and this side's reply (RFC 6455 5.5.1 lets an endpoint
  finish the message it's sending first). `close_reply()` writes the reply's payload, the peer's code alone (none for
  1005), as gorilla's default close handler does.
- The handshake: `check_request()` returns the key, and `check_response()` the subprotocol chosen, as views of the
  message's fields, so the message must outlive them (two tests first kept them past a temporary, which ASan caught).
  Over HTTP/2 `check_request()` fails with `not_websocket` until stage 8 gives `server_request` its `protocol`: the
  extended CONNECT's checks need it, so the seam is that one branch. A body is a Content-Length other than 0 or any
  Transfer-Encoding. `Sec-WebSocket-Version` must be one line, `13`; the key one line, 24 characters of strict base64
  decoding to 16 bytes. Subprotocols are compared exactly (browsers compare the server's choice so), from the server's
  list, over every `Sec-WebSocket-Protocol` line. `make_request()` gives a `client_handshake{request, key}`, setting
  the handshake's fields over the caller's and dropping a caller's `Sec-WebSocket-Extensions` (no extension can be
  taken). `check_response()` fails a missing `Upgrade`/`Connection` with `not_websocket`, the accept compared exactly.
- Origins (Decided question 7), where the plan was silent on ports: the request's own host is its Host's (or an
  absolute target's) and its port, a port left out on either side being the origin's scheme's default (80 for http and
  ws, 443 for https and wss). So a TLS-terminating proxy's backend (Host without a port, Origin https) passes, and a
  different port doesn't. Patterns are `host[:port]`: `*` for exactly one label (`*.example.com` matches neither
  `example.com` nor `a.b.example.com`), a port, `:*` for any, or none for the default (`localhost` doesn't allow
  `http://localhost:3000`; `localhost:3000` does). `null`, an origin with a path, a malformed one and several lines are
  refused; `insecure_any_origin` takes even those.
- Tests share `test/websocket/frames.hpp`: `frame()` builds a frame, and `read(session, input, piece)` reads frames
  through a session as its owner will, from input arriving `piece` bytes at a time, recording the frames taken, the
  events, and the failure with the bytes being read when it came (`failed_byte()`, read a byte at a time). The session's
  rules are one table over both roles, each case read at once and a byte at a time, which must agree. `fuzz_websocket`
  uses the same reader (as `fuzz_utf8` covers the validator, `fuzz_websocket` leaves it out).
- Each rule was broken alone (42 breaks: the codec's, the session's both ways, the handshake's and the error
  category's), and the tests failed for every one. `fuzz_websocket` (45 s each, from a corpus of ten minutes' run)
  found 14 of the 28 breaks in the codec and the session (the masked server frame, fragmented and oversized control
  frames, both minimal-length checks, a 1-byte close, a close reason that isn't UTF-8, RSV bits, reserved opcodes, the
  two continuation rules, frames after the peer's close, the mask's offset, an unmasked client's frames), through
  re-encoding and the sending session taking what the receiving one took; it can't see a rule that reading at once, a
  byte at a time and the sending side all agree on (UTF-8, an unmasked frame to a server, the length's top bit,
  `max_message`, 1005 and 1015 on the wire, dropping after this side's close, the reply's code, the sending side's own
  checks), which the table tests cover.

## Learned in stage 3

- The split between the hook and `response_writer`: the hook (`connection_state::switch_now()`, a `switch_hook`'s
  `call`, returning a `coro::task<tunnel*>`, a ready task unless it has a watch to wait for) does the connection's part
  only (the gate, the watch, the reader's hold, the timeouts), and `switch_protocols()` (a friend of `response_writer`)
  the response's: it checks what it can first (sent already: `std::logic_error`, as `send()`; HTTP/2, HTTP/1.0 and a
  `response_writer` without a hook, e.g. the server's own error responses': `not_supported`; a field that can't be sent,
  given or the response's own: `invalid_argument`), so a refusal sends nothing and leaves the handler to respond; then
  calls the hook, and once the connection's committed marks the response `switched()` and `sent()` before encoding the
  101 (so a 101 that fails to go still ends the connection, the error returned), puts `fields` over the response's own
  of the same names (dropping Content-Length and Transfer-Encoding), encodes and flushes. `switched()` is public on
  `response_writer` (middleware may ask); `switch_protocols()` stays internal. It returns `std::expected<tunnel*,
  std::error_code>`; the tunnel is the connection's, for the handler's call.
- A rule the plan didn't have: a request whose body hasn't been read to its end can't switch (`invalid_argument`,
  nothing done): what follows its head is the body until then, which the tunnel would take for the new protocol's bytes.
  The hook reads the gate's `body_read` mark (set on the handler's task, or before it for a request without a body), so
  it costs nothing. WebSockets' requests have no body (`check_request()`), so it only guards misuse.
- `switch_hook` is defined in `response.hpp` (with `tunnel` forward-declared), not `tunnel.hpp`: `response.cpp` is built
  into the fuzz targets, which don't have spdlog, and `tunnel.hpp` needs `io::clock` (`io/scheduler.hpp`) for
  `set_read_timeout()`.
- The watch, where the plan was too quick: a switch that finds the gate opened (its `fetch_or` sees `handler_waited`,
  `body_read` being known set) can't tell whether a watch was begun, as `begin_watch()` may be running on the server's
  thread (in `handler_runs::await_suspend()`, right after the handler suspended) while the handler, resumed on another
  worker, switches. So it always waits through the handoff (`switch_after_watch()`: a read deadline of now first,
  harmless if no watch begins), and `begin_watch()` declining (something buffered, as when the client sent bytes with
  its request, or stopped) publishes as a watch's end would (`no_watch()`, an exchange, resuming a waiting switch on the
  pool, as it may be inside `handler_runs`). `watch_handoff` is reset per request by the loop (a relaxed store beside
  the gate's), not by `begin_watch()`, whose store could overwrite a switch already waiting. That race isn't reachable
  deterministically, so no test forces it; the tests take each path the hook has: no watch (the handler didn't suspend),
  a watch abandoned, a watch declined (bytes sent along), and a watch that had ended (the client half-closed).
- `tunnel::abort()` also drops what's buffered: the loop's end doesn't flush an aborted tunnel's writer (an HTTP/2
  reset would drop it too), so a peer that doesn't read can't hold the connection for `write_timeout` after an abort.
  A handler that throws after switching takes the failure path as before (logged, flushed, lingering).
- `closing()` is the server's `close()` token as is; until stage 5 nothing else happens on `close()` (the tunnel goes
  on, and `join()` waits for its handler).
- `bench_request` (Release, four runs of the build before and three after, interleaved; the best of each, and the
  range): pipelined 1137 ns per request before (1137 to 1169), 1143 after (1143 to 1159); one request per read 1316
  (1316 to 1369), 1360 (1360 to 1364); over a socket 28.1 us (28.1 to 30.1), 28.4 (28.4 to 30.3); to a handler that
  waits once (so watched) 36.3 us (36.3 to 39.5), 37.3 (37.3 to 38.8). Allocations per request the same everywhere
  (1.4, 4.2, 3.0, 2.0, and the HTTP/2 and middleware lines as they were). So nothing measurable, as the plan expected:
  the pointer in `http11_backend`, the test of `switched()` after the handler (the bench's own copy of the loop has it
  too), and a relaxed store per request for the handoff.
- Each rule and path was broken alone (28 breaks: the body rule, the `switched` mark, waiting for the watch, the
  watch's deadline of now, `no_watch()`'s publication, the per-request handoff reset, the tunnel's abort (its mark,
  its shutdown), the loop's flush and its end, the input's hold, the read timeout and deadline cleared, the tunnel's
  timeout, stop token and `closing()`, Content-Length dropped, the fields merged, `sent()` and `switched()` set, the
  HTTP/1.0, field and hookless refusals, the 101's flush, the hook's refusal, and sending twice), and the tests failed
  for every one but `after_watch()` in `switch_after_watch()`, which leaves nothing observable once the switch has set
  the input up (it keeps `watching` false, as the server's paths do). Three of them hang rather than fail (the watch
  never ending, or the 101 never sent), which the test run's time limit shows.

## Learned in stage 4

- The API, where the plan left it open: `message_type` (`text`, `binary`); `connection_options{max_message,
  close_timeout}` is `accept_options::connection` (the client's `connect_options` gets it in stage 6); `accept()` takes
  the request as `const`. `begin_message()` returns a `message_writer` by value (an `io::writer` with `end_message()`),
  bound to its message by a count, so a writer kept past its message's end fails (`invalid_argument`) rather than ending
  someone else's; a task holding one must end it before writing a whole message, which waits for it. `ping(payload)`
  only sends: the pong is read as reads come to it. `close(code, reason)` takes `no_status_received` for a close without
  a code. `subprotocol()` and `close_reason()` are views of the state.
- `close_timeout` is used by `connection::close()` already (stage 5 adds the server's `close()`): `close()` sends its
  close, then reads for the peer's itself, each wait's read timeout what's left of the timeout, or, if another task is
  reading, waits for it with `scheduler::wait_on()` on a word the reader bumps when it lets the read side go (the plan's
  notifier has no deadline). `tunnel::get_scheduler()` is new, for that and for the write turn's notifier's pool. The
  wait for the write turn itself has no deadline: a write blocked on the peer is bounded by `write_timeout`.
- The write side is two turns, not one: the wire (whoever writes to the tunnel's buffered writer) and the message turn
  (held by a message written a frame at a time). Whole messages wait for both, a streamed message's frames and `ping()`
  for the wire only, which is how control frames go between a streamed message's frames even while its writer is idle.
  The read's replies go when the reader finds the wire free; otherwise the holder sends them after its frame's flush
  (then flushes again), before it lets the wire go. Only the latest ping's pong is kept.
- Failures: one sticky failure for reads and writes, the first: the peer's close as its code (so a write after the
  peer's close returns its code, not `broken_pipe`, which only writes between this side's close and the peer's get); a
  protocol failure as `sent(code)`, whose close is sent (the connection doesn't then wait for the peer's: RFC 6455 7.1.7
  has it close; the handler returns and the server closes TCP); a transport failure as `abnormal_closure` (for
  `status_condition::closed`) or the error (e.g. a write's `timed_out`), which also aborts the tunnel. Text written
  isn't checked to be UTF-8 (the peer does; gorilla and coder don't either).
- A refused handshake is answered with the error's message as a text body; the 426 also has `Upgrade: websocket` and
  `Connection: Upgrade`, as RFC 9110 15.5.22 requires of a 426. A switch that `switch_protocols()` refuses (a writer
  that isn't the server connection's, a field that can't be sent) answers nothing, leaving the handler to.
- Reading without a coroutine: `pump()` steps through what's buffered toward its goal (a message begun, a whole message,
  some of one, the peer's close), so a message that's all there is read by a plain function (a ready task); otherwise
  one coroutine, `drive()`, a template returning the operation's own result. A large frame of a whole message with
  nothing buffered is read straight into the caller's buffer (unmasked and validated there). Measured with a throwaway
  probe (`bench_request`'s counting, a peer on a blocking socket so its allocations don't count): 1000 buffered
  messages, 1 allocation in all for `read_message()` (the first read, which waited) and none for `write_message()`; read
  and echo one at a time, 4 allocations per read (`drive()`'s frame, `buffered_reader::fill_after()`'s, and two more
  in waiting for the socket, not traced further) and none per write. A contended write turn allocates (the notifier's list of waiters).
- `connection`'s destructor aborts the tunnel unless this side's close has gone. With every write flushed and no read
  left (tasks must be done by then), what the abort changes isn't observable: the peer sees the connection end without
  a close either way.
- The tests (`test/websocket/server.cpp`, `[websocket][server]`, and a TLS case in `test/tls/server.cpp`) are the
  Testing section's but the idle pings and the server's `close()` (stage 5's); `close()`'s own timeout is tested here,
  as `close()` uses it, with a short `close_timeout`. Their scripted client (`test/websocket/raw_peer.hpp`) sends from a
  task of its own alongside its reads: an echo of a 1 MiB message deadlocked until `write_timeout` while the client was
  still sending the next one.
- Autobahn's `fuzzingclient` (Docker, `crossbario/autobahn-testsuite`; config and report in a scratch directory) against
  `example/websocket-echo` in a Release build (its `max_message` 16 MiB, for section 9's largest messages), sections 1-7
  and 9: 300 cases, 297 OK and 3 INFORMATIONAL (7.1.6, 7.13.1 and 7.13.2, which only record what the server did), no
  NON-STRICT, none FAILED, the first time, and again on the commit's code (nothing needed fixing). Section 9 took 10.1 s
  in all (the whole run 16.7 s): 9.1.6 (a 16 MiB text message) 338 ms, 9.2.6 (16 MiB binary) 203 ms, the slowest 9.5.1
  (1 MiB in 64-byte frames) 951 ms and 9.6.1 901 ms. The suite's report stays out of the repository.
- Each rule and path was broken alone (22 breaks: the latest pong, the close's echo, a failure's close, 1006 for the
  transport's end, a whole message waiting for a streamed one, a stale writer, writes after the close failing rather
  than waiting, the second read refused, `close()` told when the read side's let go, `read_message()` and
  `next_message()` skipping a message's rest, the direct read unmasked and validated, `max_message`, `close()`'s read
  timeout, the turn's notification, a reply left to the wire's holder, the 426's fields, the subprotocol, the accept,
  the destructor's abort, a failure's abort, and a streamed read stopping at a frame's end), and the tests failed for
  every one but two: the destructor's abort, unobservable as above; and the streamed read's stop at a frame's end, which
  only made a read return less than it could (the rule went). A failure's abort was caught once a read waits while a
  write times out (the abort is what ends that read). The peer's close not answered hangs rather than fails, which the
  run's time limit shows.

## Learned in stage 5

- The API, where the plan left it open: `idle_timeout` is in `connection_options` (60 s; 0 off), as `close_timeout` is;
  `stop` is in `accept_options`, not `connection_options`, as the client's `connect_options::stop` (stage 6) covers its
  handshake as well as the connection, so each side has its own. A token without a stop state (the default) means
  `req.stop`, so `abort()` and `http::timeout` on the route reach the WebSocket without the handler passing anything.
- **A departure: the server's `close()` interrupts reads with a read deadline, not the transport's stop token.** The
  socket's stop token cancels writes as well as reads, so a write waiting when `close()` came (a server streaming to a
  slow client) would fail with `operation_canceled`, part of a frame sent (over TLS, a `write_failure`, sticky), and
  the 1001 could never go: the WebSocket would end abnormally on every graceful shutdown that caught it writing. So
  `tunnel` has `set_read_deadline()` (the socket's read deadline, which applies to a read already waiting and may be set
  from any thread, as the server's `abandon_watch()` uses it), and the WebSocket registers a `std::stop_callback` on
  `closing()` once, at `accept()`, setting a deadline of now. A read whose wait times out (having read nothing, so
  nothing's lost) asks `closing().stop_requested()`; if so, it unregisters the callback (waiting for it, if it's being
  made) before it takes the deadline off, so the callback can't set it again after, and goes away. No
  `util::linked_stop_source` is needed: `stop` alone is the transport's stop token. A read timing out over TLS leaves the
  stream as it was (as a canceled one does), which the TLS test checks.
- Going away is a failure set through `set_failure(sent(going_away))`, so its close is left pending as a protocol
  failure's is, and sent as a reply (by the read if the wire's free, or by the write holding it, after its frame), and
  writers waiting see the failure at once. Then the read reads on for the peer's close (`read_close()`, within
  `close_timeout` from then) and returns `sent(going_away)` whether the peer answered or not (then the connection's
  aborted), as everything does after it; `close()` afterwards returns no error if the handshake finished, or the failure
  if it didn't. A close of this side's already under way (`close_sent`) isn't disturbed: a read takes the deadline off
  and reads on, and so does `read_close()` itself, `close()`'s own timeout bounding both.
- Only a read that has to wait notices: a message that's buffered already is read first, and a WebSocket that isn't
  reading at all hears nothing until it next reads (the deadline stays set until then). (No longer: a peer that never
  let a read wait kept the WebSocket open, so every step of a read now looks; see "Fixed after stage 6".) A WebSocket
  accepted after `close()` (its request already being served) gets its 101, and goes away at its first read that waits:
  the callback runs as it's registered.
- `stop`: the transport's stop token cancels the socket's waits (reads and writes), and a callback (`cancel()`) fails the
  connection with `operation_canceled` unless it has failed already (so a stop after a clean close, or after the peer's
  close, changes nothing), wakes the writers waiting for their turn (a whole message waiting behind a streamed one whose
  writer is idle waits on nothing the token reaches), and aborts the tunnel. close()'s wait for another reader needs
  nothing more: the reader's read is canceled, and letting the read side go tells `close()`. The connection's
  destructor unregisters both callbacks and puts the tunnel's stop token, read timeout and read deadline back as it
  found them, for the server's end of the connection (whose TLS close_notify shouldn't be canceled by `req.stop`).
- The server's `abort()` callback now requests stop on the handlers' source before it shuts the reading side, so a
  WebSocket's read reports `operation_canceled` rather than racing the end of the stream to `abnormal_closure` (stage
  4's abort test expected 1006; now it's `operation_canceled`). The race didn't show locally (the old order passed 60
  runs of the test out of 60: the stream's end has to go through the event loop to a worker, which the inline callback
  beats), so the change is for what the order guarantees.
- The idle timeout: half of it is the tunnel's read timeout, set once by the connection (and put back after
  `read_close()`'s own). `drive()` sends a wait that fails to `wait_failed()`, a coroutine of its own, so a read that
  doesn't time out has no frame more; `idle_pinged` is set by the ping and cleared by any bytes read (the fill's, or the
  direct read of a large frame's, which also times out with some read: then the ping goes as well). The ping has no
  payload, and is a reply as the pong is (`take_replies()` frames the pong, the ping, then the close). A peer that
  answers but says nothing else is pinged every half.
- Allocations: the stage 4 probe (`bench_request`'s counting, the peer on a blocking socket), Release, interleaved
  before and after: 1000 buffered messages, 0.001 allocations per `read_message()` and none per `write_message()`,
  before and after; read and echo one at a time, 4.011 to 4.012 per read before and 4.013 to 4.014 after (and 4.003 to
  4.004 over 4000 rounds), none per write: two or three allocations once per WebSocket (the read timeout's timer nodes,
  which the scheduler then reuses, presumably), and nothing per read.
- Tests (`test/websocket/server.cpp`'s `[idle]`, `[cancel]` and `[going-away]`, and an idle and a going-away case over
  TLS in `test/tls/server.cpp`) assert order and outcomes, with lower bounds only for the idle timing (from before the
  request) and `close_timeout`. 25 runs in a row of the new tests, and 30 more six at a time, all passed. Each rule and
  path was broken alone (19 breaks: the ping, the idle reset by the fill and by the direct read, the second half
  failing, 0 as off, the deadline taken off, the callback's deadline, the 1001 sent, the peer's close read for, the
  state checked, `read_close()` reading on, `wait_failed()`'s check of closing, the tunnel's deadline, the ping framed,
  `cancel()` waking the turn and keeping an earlier failure, `req.stop` as the default, the transport's stop token, and
  the abort callback's order), and the tests failed for every one but the last (above); the second half never failing
  hangs rather than fails (the peer is pinged for ever), which the run's time limit shows. Not observable, so not
  tested: `cancel()`'s abort (every write flushes, so nothing's left to drop, and an abort shuts only the reading side),
  the destructor putting the tunnel back, `read_close()` putting the idle timeout back (nothing reads after it), and
  the callback unregistered before the deadline's taken off (a race).
- Autobahn's `fuzzingclient` against `example/websocket-echo` (Release), sections 1-7 and 9, as in stage 4: 300 cases,
  297 OK and 3 INFORMATIONAL (7.1.6, 7.13.1, 7.13.2), none NON-STRICT or FAILED; section 9 in 9.9 s (the run 16.5 s). By
  hand, a WebSocket open to the example when it gets SIGTERM receives a close with 1001 at once, and the example exits
  as soon as it's answered.

## Learned in stage 6

- The API, where the plan left it open: `connect(const http::client&, std::string_view url, const connect_options&)`
  takes the client as `const` (it only reads its configuration and its pool's pointer, which its copies share; the
  lint's const-correctness wants it so). It's a plain function: the URL's parsed and the options copied before it
  returns, so neither needs to outlive the task (as `client::get()` does it), and the coroutine (`open()`) takes
  everything by value. `connect_options` gets `connection` (the same `connection_options`) and `stop`, which covers
  connecting, the handshake and the WebSocket. The schemes: `ws`, `wss`, `http` and `https`, in any case; another, or
  none (`url::parse()` takes `127.0.0.1/ws` as a host and path), is `protocol_not_supported`, as a URL without a scheme
  says nothing of TLS; a URL that doesn't parse is `invalid_argument`. A fragment is dropped (the request target is the
  path and query).
- Where the connection comes from: the client's pool, through an internal friend accessor
  (`http::detail::pool_of(client)`), and a new `connection_pool::connect_apart(scheme, host, port, stop)`: the pool's
  own `connect()` (the resolver, `connect_timeout`, `attempt_delay`, the TLS handshake within `tls_handshake_timeout`),
  but never counted or kept, and over TLS with a context of its own (`http1_context()`, made on first use from the same
  `tls::client_options`, offering only `http/1.1`), or the pool's when `http2` is off (it offers only that already). So
  "the connector" the plan named is the pool's connecting half; nothing else holds a context.
- The tunnel (`client_tunnel`, in `connection.cpp`): the connection behind a `unique_ptr` (as the pool gives it), and a
  16 KiB `buffered_reader` and `buffered_writer` of its own; `closing()` gives no token, `abort()` shuts the reading
  side. The connection's state owns it (its first member, so it's destroyed last, which closes the socket), taking a
  `tunnel&` and the `unique_ptr` by rvalue reference, so the reference is bound before the pointer moves.
- The handshake: the socket's write timeout is `io_timeout` from the start (so the request's write, then the
  WebSocket's, as the plan had it), and its stop token `stop`; `request_encode_head()` into the tunnel's writer, a
  flush, then `response_decode()` on the tunnel's reader (a 1 MiB limit on the head, as the HTTP client's; interim
  responses but 101 skipped) under a read deadline of `response_header_timeout` from the flush; then `check_response()`.
  A 101's body (the rest of the connection) is never read, so what followed the head is in the reader for the WebSocket.
  Any failure once stop's been requested is reported as `operation_canceled` (as `client::send()` does: a cancellation
  can surface as another error, e.g. a TLS read cut short), and stop's checked once more before the connection's made,
  so a connection is never returned already canceled.
- Masking: a frame that fits the writer's buffer is copied into `prepare()`d space and masked there (`apply_mask()` from
  offset 0); one that doesn't goes through `send_masked()`, a coroutine that writes the header, then the payload a
  buffer's worth at a time, each part copied and masked at its offset, flushing as the buffer fills. The control frames
  (`control_frames::append()`) were masked already. Writes never touch the caller's data (tested).
- The close, where the plan was brief ("the client then waits for the server's end of the stream ... within the same
  timeout"): the wait (`await_end()`, which drops what comes and reads until the stream ends or the deadline) follows
  the close handshake wherever it finishes on a client, not only in `close()`: the read that answers the server's close
  (`drive()`'s failed step, through `end_failed()`, with `close_timeout` from then) and `read_close()` (with `close()`'s
  deadline). `close()` waiting for another task's read, once the handshake's done, waits on while that read is in
  `await_end()`, and its own deadline passing then is no error (the handshake did finish). Either way the client ends
  TCP itself only by destroying the connection, after the server's end or the timeout. Not covered: a reply to the
  server's close left to a writer holding the wire (another task writing as the close comes) is sent by that writer, and
  nothing then waits for the server's end.
- An example client beyond Autobahn's driver wasn't added: the guide's section shows a client, and
  `example/websocket-autobahn` is a small one already (connect, echo, close).
- `bench_websocket` (`bench/websocket/echo.cpp`, Release, two workers, best of five runs, three runs alike): cppnet's
  client against cppnet's server, an echo handler, allocations of both sides together.

  | Messages                     | Time per message | Messages a second | Allocations per message |
  |------------------------------|------------------|-------------------|-------------------------|
  | 16 B text, one at a time     | 27.8 us          | 36,000            | 4.00                    |
  | 16 B text, in flight         | 4.0 us           | 249,000           | 0.35                    |
  | 64 KiB binary, one at a time | 116 us           | 8,600 (537 MiB/s) | 10.0                    |
  | 64 KiB binary, in flight     | 45 us            | 22,000 (1.4 GiB/s)| 7.4                     |

  One at a time is a round trip through both schedulers and the kernel each; the 4 allocations of a small one are the
  two reads that wait (each `drive()`'s frame and the socket's wait, as stage 4 found on the server; nothing per
  write). In flight, reads mostly find messages buffered, and need no coroutine. A large message's are the large
  writes' coroutines (`send_masked()` on the client, `send_through()` and `io::write_all()` on the server, and their
  flushes that wait for the socket) as well; not traced further.
- Autobahn's `fuzzingserver` (Docker, `crossbario/autobahn-testsuite`, its port published to the host; config and report
  in a scratch directory) against `example/websocket-autobahn` in a Release build, sections 1-7 and 9: 300 cases, 297 OK
  and 3 INFORMATIONAL (7.1.6, 7.13.1, 7.13.2, which only record what the client did), none NON-STRICT or FAILED, the
  first time; every case clean, the server ending TCP first in all of them. The run took 19.5 s, section 9 10.5 s (the
  slowest 9.5.1, 1 MiB in 64-byte frames, 1.2 s); again on the commit's code, the same counts, section 9 in 9.2 s. The
  suite doesn't see the client's wait for the server's end of TCP: with it taken out, still 297 OK, the server ending
  TCP first in all 300 (it ends it as soon as it has the client's close, and the driver's connection is destroyed only
  after its read returns), so the loopback tests are what check it.
- Tests (`test/websocket/client.cpp`, `[websocket][client]`, and `[tls][http][client][websocket]` in
  `test/tls/server.cpp`): against cppnet's server, echo (three spellings of the scheme; a 300 KB message and a streamed
  one), subprotocols, the server's close ending a read, stop before connecting, during the handshake and during a read;
  against a scripted server (`raw_server` in the test, over `raw_peer`), the handshake's request, the refusals the plan
  listed and more (no Upgrade, not HTTP, the connection ending), connecting's failures, frames in the 101's write, the
  frames' masks, the close in four orders (close() alone, close() with another task reading, the server's close, and
  close() called while the read that answered the server's waits for its end), each with the server ending TCP after a
  pause or never, and writes timing out at `io_timeout` with a read waiting; and wss:// to a server that speaks HTTP/2,
  the client's `http2` on and off. Assertions on order and outcomes, with lower bounds only on time (and one upper
  bound, below a 2 s `close_timeout`, for a server that ends TCP after 100 ms).
- Each rule and path was broken alone (23 breaks: masking in the buffer, a large frame sent unmasked, `send_masked()`
  not masking and masking from offset 0, the tunnel's abort, the wait for the server's end, `close()` waiting for a
  reader in it, `close()`'s deadline once the handshake's done, `await_end()` dropping what comes and its timeout, the
  pool's TLS context offering h2, the write timeout, the handshake's stop token, failures reported as canceled once
  stopped, `response_header_timeout`, the last stop check, `http://` and no scheme, the client's role, the response's
  check, the frames after the 101, the default port, and the context of its own when `http2` is off), and the tests
  failed for every one but four: failures reported as canceled once stopped (over TCP a canceled wait already says
  `operation_canceled`; it's for a cancellation that surfaces as another error, as `client::send()` has it), the last
  stop check before the connection's made (only a race reaches it), the default port (80 and 443 can't be listened on in
  the tests), and `http1_context()` reusing the pool's context when it offers only http/1.1 (only memory). The tunnel's
  abort and `close()`'s wait for a reader in `await_end()` were caught once the tests had a read waiting while a write
  timed out, and a `close()` called after the read had answered the server's close; `response_header_timeout` taken out
  hangs rather than fails (the scripted server waits for the client), which the run's time limit shows; and
  `await_end()` not dropping what comes never ends once the buffer's full (the tests' 10 s limits fail it).

## Fixed after stage 6: the server's close and a busy peer

- The gap: the server's `close()` reached a WebSocket only through the read deadline of now, which a read meets only
  when it has to wait. A peer sending nonstop (small messages, one endless message of frames, or empty continuation
  frames, which never reach `max_message`) always left something buffered or ready on the socket, so the read never
  waited, the WebSocket never went away, and `server.join()` waited for ever: a client could keep the server from
  closing. Before the fix, every busy test below failed once the client's 5 s read timeout passed.
- The fix: the callback (`interrupt`) also sets a flag of the connection's own (`server_closing`, an
  `std::atomic_bool`), before the deadline, and `pump()` looks at it at the start of each step, returning
  `pumped::closing`; `drive()` takes that to `go_away()` (what `wait_failed()` did when the deadline came: the 1001 as a
  reply, then `read_close()` within `close_timeout`, or nothing if a close of this side's is under way), and
  `read_close()` takes it and reads on. A wait that the deadline ends now only goes on to the next step. A step takes no
  more than what one read of the transport brought (a buffer's worth, or one direct read into a large frame), so a read
  in progress sees the close within that, however busy the peer; and `read_close()` already checked the clock before
  each fill, so a peer that sends on is cut off at `close_timeout` (a test breaks that check, and fails).
- Why a flag, not `closing.stop_requested()`: the stop state's word is the server's, written by every connection's
  callback as it's registered and unregistered (HTTP/1.1 connections' and WebSockets'), so loading it on each step would
  share a cache line that connections coming and going write. The flag is relaxed: it carries nothing but itself
  (`take_closing()` unregisters the callback, which waits for it, before taking the deadline off and clearing the flag),
  and it's set before the deadline, so a read that the deadline wakes (through the socket's lock) sees it. It sits in
  padding beside `reading` and `close_waiting` (after the other read side's state, it set off clang-tidy's padding
  check).
- The behaviour changed: a message that's buffered already is no longer read first. Once the server's close has come,
  the next step of a read goes away, and messages not yet read, buffered or not, are dropped, as those that come after
  it always were. `message()`'s read after its message's end still returns the end of the stream first (it reads
  nothing). The connection's comment and the guide say so.
- What else a peer could hold open, checked: `close()`'s own `read_close()` (the same clock check; and the server's
  close under it now taken by a step as well as by a timed-out fill), `close()` waiting for another reader (its
  `wait_on()` has the deadline), streamed reads (each `message().read()` is a step), whole reads of a large message
  (each fill or direct read is followed by a step), skipping (inside a step), and the client (no `closing()`; its wait
  for the server's end checks its deadline each round). Not changed: a handler that isn't reading hears nothing (as
  before, documented), and a write held by a peer that doesn't read is bounded by the server's `write_timeout`, not by
  `close_timeout` (an abort shuts only the reading side), so `join()` may wait that long for a handler that awaits its
  own writer. The idle timeout's ping is untouched (`wait_failed()` looks at the flag first, as it looked at the token).
- `bench_websocket` (Release), before and after, interleaved, 15 runs each while another worktree's tests loaded the
  machine (so the medians are noise), best of each: small in flight 4.66 and 4.65 us per message, one at a time 32.88
  and 32.46 us; large in flight 51.36 and 51.58 us, one at a time 136.56 and 135.98 us; allocations the same (4.00, 0.18
  to 0.37, 10.0, 7.3 to 7.7). The flag's load doesn't show.
- Tests (`test/websocket/server.cpp`): `[going-away][busy]`, a peer that never lets the reads wait (`send_nonstop()`,
  64 KiB batches of frames, from a task of its own), three shapes (small messages read whole, an endless message of
  4 KiB frames streamed, an endless message of empty frames read whole), each answering the 1001 once seen (`join()`
  well within a 5 s `close_timeout`) or sending on (the connection cut at a 300 ms `close_timeout`, as a lower bound);
  the going-away test of a WebSocket that isn't reading gets a message sent before the close (dropped: the read returns
  1001, where it returned the message before the fix); and the close-in-progress test gets a peer sending nonstop as the
  server closes (the handler's close finishes, with no 1001; this one passes without the fix too, and is there for the
  paths the fix adds). 25 runs in a row and 30 six at a time, then 25 more after the flag moved into padding, all
  passed. Each part was broken alone (11 breaks: the step's check, the callback's flag, the wait going on to the step,
  `drive()`'s `go_away()`, `go_away()` taking the close, its state check, its `read_close()`, `read_close()`'s `closing`
  case and its timed-out fill, the flag cleared, and `read_close()`'s clock check), and every one was caught: eight
  failed, and three (the step's check, `read_close()`'s case and the flag cleared) hang rather than fail, a read
  spinning without ever suspending, which the run's time limit shows.

## Learned in stage 7

- The setting: `settings::enable_connect_protocol`, a field like the others (so `encode_settings()` sends it when it's
  1, and `peer_settings()` is how a client reads the server's, which stage 9's pool will check). A server's session
  sends it when its `session_config::local.enable_connect_protocol` says so (off by default; nothing sets it until
  stage 8's `server_config::http2_websockets`); a client's session clears it, as it forces ENABLE_PUSH to 0, since it
  means nothing from a client. Its value must be 0 or 1 from either side (PROTOCOL_ERROR otherwise): RFC 8441 3 says so
  of any sender, and Go's `Setting.Valid()` and nghttp2 check it whatever the side, so "ignored from a client" is its
  meaning, not its range. A client takes 1 then 0 as PROTOCOL_ERROR, within one SETTINGS frame too (settings apply in
  order); a later SETTINGS that leaves it out changes nothing, and 1 after 0 (or after none) is taken, as nothing
  forbids enabling it later.
- `:protocol` reaches the events as a pseudo-header among the fields, in the order sent: `session_events` didn't
  change, and stage 8's server reads it as it reads `:method`. The value must be a token (Upgrade's protocol names
  are; an empty one, or one with a space, is malformed). With `:protocol`, CONNECT needs `:scheme`, a non-empty `:path`
  (as other requests' must be) and `:authority`; RFC 8441 4 names the first two, and `:authority` is CONNECT's.
- The client's side: `submit_headers()` refuses a request with `:protocol` until the server has sent 1, with
  `invalid_argument` (as for other things the session won't send), sending nothing, and forgets the stream rather than
  resetting it: the server has never seen it, so an RST_STREAM would be on an idle stream, a connection error to it
  (RFC 9113 6.4), and `http2_client_connection::open()` resets a stream whose head fails, which then does nothing. Only
  `:protocol`'s presence is checked: the session doesn't otherwise check what its own side sends (e.g. `:protocol` on
  GET). The block's scan moved into `read_request_head()` (method, `:protocol`, the Priority lines), as
  `submit_headers()` had grown past the lint's cognitive complexity.
- How CONNECT streams end, which the plan asked to check, needed more than `:protocol`: a CONNECT request's
  Content-Length bounded its stream's DATA (the session reset a stream whose DATA ran past it), and trailers were taken
  on it. `stream_state::head_request` became `kind` (`exchange`: plain, head, connect, tunnel; one byte, as before),
  and a CONNECT's stream is a tunnel: a server's from its request (plain or extended), a client's from a 2xx to its
  CONNECT (`connect` until then). A tunnel's DATA is the tunnel's, not content: no Content-Length holds it (RFC 9110
  9.3.6: a CONNECT request has no content, and a client MUST ignore a 2xx's Content-Length), and HEADERS after its head
  is a stream error (RFC 9113 8.5: only DATA and the frames that manage a stream follow). A non-2xx response to CONNECT
  is an ordinary response: its Content-Length holds, and trailers may follow. A CONNECT request's Content-Length is
  ignored rather than refused (nghttp2 ignores it for plain CONNECT, but holds extended CONNECT to it, which resets a
  WebSocket whose client sent `content-length: 0`; Go holds both to it). For stage 8: the fields are reported as they
  came, so a Content-Length on an extended CONNECT reaches the server unenforced, and the server shouldn't present it
  to a handler as the body's length when it shows the request as GET (drop it, or refuse the request).
- `fuzz_http2`: the server's session sends ENABLE_CONNECT_PROTOCOL = 1; the client's starts with the server's SETTINGS
  without it, so its extended CONNECTs are refused until the script's SETTINGS say 1. New records: op 2's bit 7 has a
  client open an extended CONNECT or a plain one; op 7 with bit 7 (and neither 5 nor 6) receives SETTINGS with
  ENABLE_CONNECT_PROTOCOL of the next byte, or a HEADERS frame encoded with literals (so it decodes whatever the
  session's table holds): a server a request (an extended CONNECT, without `:path`, `:scheme` or `:authority`, GET with
  `:protocol`, a plain CONNECT, an extended one with a Content-Length, or trailers) on the peer's next stream or a known
  one, a client a response (200, with a Content-Length, 404 with one, 103, trailers, or 200 with `:protocol`). The
  existing properties hold (a refused CONNECT's block isn't among those submitted, so its sending anything would break
  `blocks_decode()`). Run for 5 minutes (87,452 runs, no failures), and again for 10 minutes on the commit's code
  (74,741 runs, no failures; the debug build with ASan and UBSan runs about 125 a second).
- Tests (`test/http/http2.cpp`, `[http][http2][session][connect-protocol]`, on bytes): the setting sent by a server
  that says so and not by default, never by a client; read by a client (0, 1, left out, 2, 2^32-1, 1 again, 1 then
  left out, 1 then 0 across frames and within one, 0 then 1, 1 then 2), ignored by a server but for a value past 1;
  extended CONNECT taken and reported, DATA both ways and END_STREAM each way; refused without the setting, on GET and
  POST, without `:scheme`, `:path` or `:authority`, with an empty `:path`, an empty `:protocol` or one that isn't a
  token, two `:protocol`s, `:protocol` after a regular field, Host disagreeing, a connection-specific field; plain
  CONNECT's rules with and without the setting; a server's tunnel (plain and extended) taking DATA past one
  Content-Length and ending short of another, a head ending the stream with a Content-Length, trailers reset, and a
  POST beside it still held to its Content-Length; a client's extended CONNECT refused before the server's SETTINGS,
  without the setting and with 0 (nothing sent, the stream gone, a reset of it sending nothing), then sent once it's 1,
  a plain CONNECT sent without it, all of it received by a server's session without an error; a 2xx (200 and 299) to a
  client's CONNECT ignoring its Content-Length, trailers after a 2xx (after a 103) reset, and a 404's Content-Length
  held (DATA past it reset) and its trailers kept; and a response to HEAD ending short of its Content-Length with an
  empty DATA or trailers (the `ended_short()` exception `kind` now carries, which nothing tested). The existing
  malformed-request case for an unknown pseudo-header used `:protocol`, which the client's session now refuses to
  send: it uses `:unknown`.
- Each rule was broken alone (28 breaks: the setting's encoding, a client's cleared, its range, a server ignoring a
  client's, withdrawal, a client recording it, `:protocol` as a server's pseudo-header, the setting, the method, the
  token, `:authority`, `:scheme`, `:path` and an empty one, a server's CONNECT's Content-Length and its tunnel, trailers
  on a tunnel, a client's 2xx's Content-Length, its tunnel and only for a 2xx, a client's CONNECT noted, the client's
  refusal, the refused stream forgotten and nothing sent, a stream's kind kept across blocks and taken from its head,
  and HEAD's two exceptions), and the tests failed for every one, once the HEAD test above was added for
  `ended_short()`'s, which none caught at first.

## Learned in stage 8

- The presentation, where the plan left room: `server_request::protocol` is the `:protocol` as sent (`websocket`,
  compared ignoring case, as RFC 9110 7.8 compares protocol names; another is 501 without a handler); the request is
  GET, its target `:path`; and its `:authority` is added as Host if it has none, as an HTTP/1.1 request would have it,
  because the WebSocket's origin check needs the request's host and port, which `req.host` alone (no port) can't give.
  `start_stream()` reads the block through `take_fields()` (the pseudo-headers and the priority signals, moved out so
  the function stayed under the lint's cognitive complexity) and `present_extended_connect()` does the rest.
- Content-Length (the user's decision, neither dropping the field nor refusing any): a CONNECT has no content (RFC 9110
  9.3.6) and its stream's DATA is the WebSocket's, which the session doesn't hold to a length (stage 7), so a
  Content-Length other than 0 means a sender that believes a body follows where none does, the disagreement request
  smuggling exploits: 400, as other malformed requests. `Content-Length: 0` says only what's true and reaches the
  handler as is, a GET without a body, as the HTTP/1.1 handshake's `check_request()` allows it (a body refused, 0
  passed). Repeated values that disagree never get this far: the session's field checks make them a stream error
  (PROTOCOL_ERROR), as for any request. Prior art: nghttp2, Go's `x/net/http2` and h2 (Rust) all hold an extended
  CONNECT's DATA to its Content-Length like any request's, so a WebSocket that carries one breaks once its data passes
  it; Envoy strips `content-length: 0` when it turns an HTTP/1.1 upgrade into an extended CONNECT, because nghttp2 would
  reject it.
- The guard: a stream that came as an extended CONNECT (`http2_stream::extended_connect`) has `http2_send_head()` refuse
  a final 2xx (`not_supported`, as `send(101)` does), so `send()` throws and a handler that lets it go gets a 500 (the
  server's own, which isn't a 2xx); an interim response, or any other status, goes as usual. A handler that returns
  without answering gets its stream reset (INTERNAL_ERROR) rather than the server's default 200, which the guard would
  refuse anyway (the explicit check says why; with it taken out, the guard's throw ends the same way, so no test can
  tell them apart).
- `switch_protocols()` routes to `open_tunnel` over HTTP/2 rather than staying HTTP/1.1's: one entry point for
  `accept()`, the response marked `switched()` and `sent()` alike, and the existing refusal (an HTTP/2 request that
  isn't an extended CONNECT, `not_supported`) kept, now `open_tunnel`'s. The fields are put over the response's own by
  `detail::put_fields()`, shared with the 101 (inside `open_tunnel`, after its checks, so a refused switch leaves the
  response as it was). `open_tunnel` is a plain function (submitting a head needs no coroutine); `switch_protocols()`
  wraps its result in a ready task.
- The tunnel (`detail::stream_tunnel`, in `http2_connection.hpp`, so the client's stage can reuse it): its own
  `stream_reader` (not `req.body`, which a handler may replace) and the stream's `stream_writer`, through 4 KiB
  `buffered_reader`/`buffered_writer`s; owned by the stream (`http2_stream_base::tunnel`), so the reader, the writer and
  the timer find it under the connection's lock. Its read limits and stop flag are plain fields under that lock (no
  atomics, so no memory order to argue: the callback and the reads both take the lock). `stream_reader::read()` asks
  `check_wait()` before each wait: stopped is `operation_canceled`, past the deadline or the read's timeout (from its
  first wait) `timed_out`, having read nothing, the stream going on; otherwise it sets `wake_at`, which the timer's
  `check_streams()` turns into a notification of `changed` (and `wake_timer_at()` wakes the timer if it's sooner than it
  knew). `set_read_deadline()` notifies `changed` itself, so the server's close (a deadline of now, from another thread)
  ends a waiting read at once. The stop callback takes the lock (never registered nor removed holding it; the server
  clears the token before the stream's destroyed under the lock) and notifies; a write that would wait (or waits queued)
  once stopped fails with `operation_canceled` and leaves the scheduler, so the writer never touches its span. `abort()`
  fails the stream (`connection_aborted`) and resets it (CANCEL).
- A departure: a tunnel's failures don't stop its handler's token (`fail_stream()` skips it). The WebSocket's default
  stop is `req.stop`, and an HTTP/2 stream's failure (the client's reset, a write timing out, the connection's end)
  stopped it, so the WebSocket reported `operation_canceled` (its canceler won the race against the read) where over
  HTTP/1.1 it reports 1006 or `timed_out`. Now the tunnel's reads and writes find the failure, as a switched HTTP/1.1
  connection's do, and the server's `abort()` still stops the token. A peer's RST_STREAM on a tunnel, whatever its code,
  is `connection_reset` (the transport's end, as TCP's reset is), so the WebSocket sees `abnormal_closure`.
- The drain: HTTP/2's graceful drain (GOAWAY, PING, the final GOAWAY) is begun only by the server's `close()`, whose
  token is the tunnel's `closing()` (`http2_server_connection::closing_token`), so the WebSocket's 1001 comes with it;
  the connection then ends once the WebSocket's handler returns. A client's GOAWAY doesn't end the streams it has, so
  WebSockets go on (RFC 9113 6.8); the connection's end (a connection error, the client closing) fails the stream, which
  the WebSocket's reads find (1006).
- What a busy or silent peer can do on a stream, checked: one sending nonstop is the "Fixed after stage 6" case (the
  flag at each step, `read_close()`'s clock), the stream's window only pacing it; one that never reads holds the
  stream's window, so a write waits, and the 1001 is left to the writer holding the wire; then `read_close()` times out
  at `close_timeout` and aborts, and the abort resets the stream, which fails the waiting write: `join()` returns at
  `close_timeout`, not `write_timeout`, better than HTTP/1.1, whose abort shuts only the reading side. The stream's
  `write_timeout` still resets a stream whose writes make no progress.
- When the handler returns, `http2_finish()` ends a tunnel with an empty DATA frame (END_STREAM: trailers are dropped,
  as HEADERS can't follow a tunnel's head) and `run_stream()` resets it (NO_ERROR) if the client hasn't ended its side,
  as for any response that's done before its request. A WebSocket that fails (the client's END_STREAM without a close,
  an idle timeout, `close_timeout`) aborts, so its client sees RST_STREAM(CANCEL) rather than END_STREAM.
- `test::h2_tunnels` (`test/http/h2_tunnels.hpp`) drives an `h2_client` from a reader and a writer task of its own, so
  each stream is an `io::reader`/`io::writer` that `raw_peer` scripts a WebSocket over, its DATA consumed only as it's
  read (so a stream that isn't read holds the server's writes), every wait limited to 10 s; `with_tunnels()` finishes it
  (ending its tasks) even when the test throws. `h2_client` gains `extended_connect()` and `settings()`.
- Tests: `test/http/server_tunnel.cpp`'s `[http][server][tunnel][http2]` (the tunnel itself: echo and the 200's fields,
  trailers dropped, the stream's read timeout off and the tunnel's own, a deadline set from another task, the stop token
  canceling a waiting read and a write waiting for the window, `abort()`, `write_timeout`),
  `test/websocket/server.cpp`'s `[websocket][server][http2]` (the setting advertised and not; echo whole and streamed,
  each length form, a byte per DATA frame; the GET presentation and the same route over HTTP/1.1; the 200's fields and
  the origin check; refusals: another protocol, a plain GET, Content-Lengths of 1, 1048576 and 0 with 1, another
  version, another origin, each followed by a request on the connection; Content-Length 0, once and twice, accepted; the
  guard: a 2xx let go (500), caught (then a 404 after a 103), none (reset); the close both ways, the client's
  END_STREAM, its reset, no close; the idle ping, answered past `body_read_timeout`, and not; abort, the stop token
  during a read and during a write; the server's close with and without an answer; a busy peer, three shapes, answering
  or not; a peer that never reads; `http2_max_concurrent_streams`; three WebSockets and requests on one connection), and
  `test/tls/server.cpp`'s `[tls][http][server][http2][websocket]` (ALPN h2: echo, and the server's close). Run 400 times
  eight at a time (the `[http2][websocket]` and `[http2][tunnel]` tests together), they found two races in the tests,
  fixed at their source: a client that reset its stream returned (and the scheduler's stop aborted the server) before
  the handler had seen the reset, so it now waits for the handler's end; and the idle test timed the ping from after the
  200 had arrived, so it now times it from before the request, as the HTTP/1.1 one does. Then 400 out of 400 passed.
- `bench_request` (Release, the build before and after, interleaved, four runs each and more for the noisy row; its
  HTTP/2 rows are bimodal on this laptop, each build landing in either mode, so the best of each): a stream at a time
  82.6 us per request before and 83.3 after, 49.3 to 49.6 allocations per request both; 8 streams at once 1223 and 1235
  us per round, 97.4 to 98.0 allocations per response both; 8 request bodies at once 1461 and 1445 us per round (most
  runs of both about 1670), 151.3 to 152.1 allocations per request both. The first response's bytes grew by 6 (132 to
  138: ENABLE_CONNECT_PROTOCOL in the server's SETTINGS); the steady state's are the same, 21 up and 40 down. So
  `start_stream()`'s `:protocol` and the reader's tunnel checks don't show.
- Each rule and path was broken alone (35 breaks: the setting from the config; only `websocket` taken; a non-zero
  Content-Length refused; 0 passed; GET; `protocol` set; Host from `:authority`; the 2xx guard; no default 200; the
  tunnel only for an extended CONNECT; its head without END_STREAM; the stream's read timeout off; trailers dropped; the
  closing token kept; the stop token cleared before the stream's destroyed; the deadline's notification; the stop flag
  ending a read; the timeout ending it; the timeout from the read's first wait; the deadline counted; the timer waking a
  read; `wake_timer_at()`; the stop callback's notification; `set_stop_token()` clearing the flag; a queued write
  canceled, and one about to queue; the abort's reset and its failure; a tunnel's failure not stopping its handler's
  token; a peer's reset as `connection_reset`; `check_request()`'s HTTP/2 version, protocol and body; `accept()`'s
  fields over HTTP/2; `switch_protocols()` marking the response sent), and the tests failed for every one but two: no
  default 200 (the guard then refuses the server's 200, which ends the same way, the stream reset), and the stop token
  cleared before the stream's destroyed (only a race reaches it: a stop callback running on another thread as the stream
  goes, which would deadlock on the lock). The abort's failure taken out hangs rather than fails (a read waits for
  ever), which the run's time limit shows. Three needed a test first: the write cancels (the first test's write was
  canceled by one branch or the other, whichever was broken, so it now waits for the handler's write to return before
  reading anything, and writes again once stopped), and `check_request()`'s body rule over HTTP/2 (unreachable through
  the server, which refuses a body before the handler: `test/websocket/handshake.cpp` now tests `check_request()`'s
  HTTP/2 form directly).
- Browsers (2026-10-30): headless Chrome 156 (driven over its DevTools protocol, with a NetLog) and Firefox 143 (over
  WebDriver BiDi, with `MOZ_LOG=nsHttp:5`), against `example/websocket-echo` with `--cert`/`--key` (a self-signed
  certificate for localhost, accepted by each browser's flag). Each first loaded `https://localhost:9443/` (a 400 over
  h2), so it had an HTTP/2 connection, then opened `wss://localhost:9443/` on it: Chrome's NetLog shows
  ENABLE_CONNECT_PROTOCOL = 1 received, and `:method CONNECT`, `:protocol websocket` on stream 5 of the page's session,
  answered `:status 200` and `date` only; Firefox made an `Http2StreamWebSocket` with `:protocol websocket` on stream 7.
  Both echoed a text message, a binary one and 200 KB of text, and closed cleanly (1000). Neither sent a Content-Length;
  both offered `permessage-deflate`, which the server ignores. From `about:blank` (no connection to the server yet),
  they differ: Chrome connects for the WebSocket over HTTP/1.1 (101), using HTTP/2 only on a connection it has already;
  Firefox connects offering h2 and opens the WebSocket as an extended CONNECT on the new connection.
- Not done: a `bench_websocket` case over HTTP/2, as cppnet's client can't open one until stage 9; Autobahn, whose
  `fuzzingclient` speaks HTTP/1.1 only.

## Learned in stage 9

- The user's decisions (2026-10-30), recorded in The client above: `connect_options::http2` on by default; without a
  usable HTTP/2 connection, HTTP/1.1 on a connection of its own (Chrome's rule; it never connects through the pool to
  find out); and only the WebSocket host's own connections, never a coalesced one.
- Which connection: `connection_pool::reserve_tunnel(scheme, host, port)` looks only at the host's own key's HTTP/2
  connections (so coalescing never applies, and there's no 421 path), and takes the first whose
  `reserve(extended_connect = true)` works: `reserve()`'s checks (not ending, no GOAWAY either way, a stream free
  counting those reserved) plus `session.peer_settings().enable_connect_protocol`, which is false until the server's
  SETTINGS have come, so one whose SETTINGS are still to come isn't used (RFC 8441 3), and can't be withdrawn once 1
  (stage 7), so the reservation can't be refused for it later. A connection with no stream free isn't waited for: the
  WebSocket goes over HTTP/1.1 apart, as it would with no HTTP/2 connection at all. Waiting (as
  `strict_max_concurrent_streams` would for a request) could hold a WebSocket's open for as long as other WebSockets
  live, which a request's short stream wouldn't; and browsers fall back the same way (Chrome uses HTTP/2 only where it
  has a stream). The stream lives as long as the WebSocket does, so it holds one of the server's MAX_CONCURRENT_STREAMS
  slots all that while (the server counts it against `http2_max_concurrent_streams` too).
- `http2_client_connection::open_tunnel(request, protocol)` (internal): `open()` gained `protocol`, sent as `:protocol`
  right after `:method` (the client's session refuses it before the server's 1, stage 7, which the reservation has
  checked); the head goes without END_STREAM and without a known length (`data_left` none: a tunnel's DATA has none).
  Then it waits for the head as `wait_for_head()` does, but simpler: interim responses are dropped as they come (the
  stream doesn't want them), `response_header_timeout` through the stream's read deadline (the timer resets it,
  `timed_out`), and `request.stop` cancels it until the head (a `canceler`, as `send()`'s). A 2xx (RFC 8441 5: any)
  makes the stream a tunnel under the lock: `read_timeout` off and `read_deadline` cleared (the WebSocket's idle timeout
  replaces it, as on the server), its `stream_tunnel` (stage 8's, with no closing token: a client has no server closing
  around it), handed out in an `http2_tunnel` that holds the stream and the connection, as `http2_response_body` holds a
  response's; its write timeout stays `io_timeout`, the WebSocket's write timeout over HTTP/1.1 too. Any other status is
  returned without a tunnel, and the stream's done with (`done()`: reset, CANCEL, unless both sides ended it). A
  `refused_stream` the server sent before processing the request (or the connection refusing to open the stream, e.g.
  after a GOAWAY that came meanwhile) is passed on as is, and `connect()` then goes over HTTP/1.1 instead, as `send()`
  retries such a request elsewhere; one after processing becomes `cancel`, as in `wait_for_head()`. `send()`'s path for
  a stream that wasn't opened became `not_opened()`, shared.
- The request's fields: `make_http2_request()`, beside `make_request()` (both from `base_request()`: the caller's
  headers, Sec-WebSocket-Extensions dropped, Version 13, the subprotocols on one line, Origin): CONNECT, version 2, and
  no Upgrade, Connection nor Sec-WebSocket-Key, even the caller's (the first two `append_fields()` drops anyway, as
  connection-specific; the key would mean nothing). The caller's Host becomes `:authority`, as the HTTP client's does
  for any request. `check_response()` got an HTTP/2 form, as `check_request()` did in stage 8: for a response whose
  version is 2, any 2xx, without Upgrade, Connection or an accept (the key's ignored), and the extension and subprotocol
  checks as over HTTP/1.1; anything else `bad_status`.
- The transport: the client's connection state owned a `client_tunnel` (its own connection); it now owns a
  `client_transport` (an internal base with a virtual destructor, `get()` for the tunnel and `close_sent()`), either the
  `client_tunnel` (HTTP/1.1, as before) or a `stream_transport` (HTTP/2: the `http2_tunnel`, whose `get()` is the
  stream's `stream_tunnel`). Still the state's first member, so it goes last: the connection closed, or the stream done
  with (and, with it, the client's hold on the pool's connection).
- Who closes: RFC 8441 5 has END_STREAM as an orderly TCP close, and RFC 6455 7.1.1 has the server close TCP first, so
  that the client doesn't hold TIME_WAIT. A stream has no TIME_WAIT, and stage 8's server resets a stream
  (RST_STREAM(NO_ERROR)) whose client hasn't ended its side when the handler returns, which it does right after the
  close handshake. So the client ends its side of the stream as soon as its close has gone (whichever side closed
  first): `write_replies_or_release()`, letting the wire go after a flush with this side's close sent, tells the
  transport `close_sent()`, which over HTTP/2 is `stream_tunnel::end()` (an empty DATA with END_STREAM, after the
  close's DATA, usually in the same round of the writer's) and over HTTP/1.1 nothing. Nothing can be sent after this
  side's close anyway (the session refuses it). Then `await_end()` waits, as over HTTP/1.1, for the server's end of the
  transport, which on a stream is its END_STREAM (the stream's reader returns the end of the stream), within
  `close_timeout`: the server sends it when its handler returns, so the wait is short, and the WebSocket's then
  destroyed with both sides ended, so `done()` resets nothing. The tests check (with a scripted server) that the
  client's END_STREAM follows its close before the server answers, and that no RST_STREAM ever comes. Destroyed before
  then, or without a close, the stream is reset (CANCEL), as a WebSocket's TCP connection would be cut.
- Lifetime: the `http2_tunnel` holds the connection (a `shared_ptr`), so it stays open while the WebSocket lives, as a
  response's body being read keeps it: the pool doesn't count it idle (it has a stream), so doesn't reap or evict it,
  and the pool's destruction (the `http::client` going first) drains it (GOAWAY, then it ends once its last stream is
  done). Its callbacks hold the pool weakly, so they do nothing once it's gone. Tested: the client destroyed with a
  WebSocket open, which echoes and closes cleanly after (with ASan).
- The connection's failures reach the WebSocket as on the server: the base's `on_stream_reset()` makes a tunnel's reset
  `connection_reset` and the connection's end fails its streams (`connection_aborted`), both `abnormal_closure` (1006);
  the server's END_STREAM without a close is the end of the stream, 1006 too. A GOAWAY from the server doesn't end the
  streams it has: the WebSocket goes on, and the connection ends with it. The server's `close()` reaches it as its 1001
  (stage 8's going away, through the stream), and its `abort()` as a reset (1006).
- `connect_options::stop` covers the stream's opening (`open()` checks it, sending nothing if it's stopped already, the
  reservation given back), the head (the canceler resets the stream, CANCEL), and, kept, the WebSocket (the tunnel's
  `set_stop_token()` and `abort()`, which resets the stream, CANCEL: stage 8's).
- Departures: none from the user's decisions. One addition to the plan's list: falling back to HTTP/1.1 when the
  stream's refused before the server processed it (REFUSED_STREAM, or the connection's GOAWAY racing the reservation),
  as the HTTP client retries such a request; a stream the server did process isn't retried.
- `bench_websocket` (Release, two workers, best of five runs, three runs alike) gained the same loads over HTTP/2: a
  second client with prior knowledge, whose GET makes the connection the WebSocket's stream then shares (cleartext).
  Stage 6's HTTP/1.1 numbers, measured again beside them, haven't moved.

  | Messages                     | HTTP/1.1, time per message | HTTP/2      | HTTP/1.1, allocations | HTTP/2 |
  |------------------------------|----------------------------|-------------|-----------------------|--------|
  | 16 B text, one at a time     | 27.4 to 27.9 us            | 51 to 56 us | 4.00                  | 12.9   |
  | 16 B text, in flight         | 4.1 us                     | 1.0 to 1.1 us | 0.37                | 0.13   |
  | 64 KiB binary, one at a time | 115 to 119 us              | 222 to 263 us | 10.0                | 83.7   |
  | 64 KiB binary, in flight     | 46 us (1.36 GiB/s)         | 165 to 178 us (about 370 MiB/s) | 7.4 | 77.3 |

  One at a time, HTTP/2 costs a round trip about twice HTTP/1.1's: each message's frames go through the stream's
  writer, the connection's writer task and its reader task on each side (a hand-over per task, where HTTP/1.1's socket
  is read and written by the WebSocket's own task), and its allocations are those tasks' waits. Small messages in
  flight go four times faster: the connection's writer gathers many messages' DATA into each write, and its reader
  takes many in each read, where HTTP/1.1 writes each message's frame with a write of its own. Large messages are
  three to four times slower, with about 80 allocations each: a 64 KiB message goes through the tunnel's 4 KiB buffers
  (stage 8's choice for the server, which the client shares) as 16 writes and reads each way, each through the
  stream's flow control (a stream window of 64 KiB, given back as it's read), many of them waiting. Not traced further;
  larger tunnel buffers for the client, or writing large payloads past the buffer, are what to look at if WebSockets
  over HTTP/2 carry large messages in practice.
- Tests: `test/websocket/client_http2.cpp` (`[websocket][client][http2]`, 20 test cases): against cppnet's server over
  cleartext with prior knowledge, a GET first so the client has the connection: echo (a 300 KB message, a streamed one),
  the request as the server presents it (GET, `protocol`, the caller's fields, no key, Upgrade or extensions), a GET
  beside the WebSocket and after its close, all on one connection; HTTP/1.1 apart with `connect_options::http2` off, a
  server not advertising the setting, and no connection yet (and HTTP/2 with all of them on); no stream free
  (`http2_max_concurrent_streams` 1: a second WebSocket over HTTP/1.1, a third over HTTP/2 again once the first's
  handler has ended); a connection whose server's SETTINGS haven't come (a listener that reads only the client's
  preface: the WebSocket connects again, over HTTP/1.1); a 403 (another origin) as `bad_status`, the connection going
  on; the server's `close()` (1001) and `abort()` (1006); stop before the stream's opened (nothing sent) and during a
  read (the server sees 1006, the connection goes on); the `http::client` destroyed while the WebSocket's open; a quiet
  stream past the client's `io_timeout`. Against a scripted server (`test::h2_client` as the server, allowing extended
  CONNECT): the request's pseudo-headers and fields; a 103 skipped; frames both ways (the client's masked); the client's
  END_STREAM after its close, whichever side closed first, before the server's answer if the client's went first; no
  RST_STREAM from the client, even once it's destroyed; refusals (404, 302, a subprotocol or an extension not offered)
  resetting the stream (CANCEL); `response_header_timeout` and stop during the handshake (CANCEL); REFUSED_STREAM
  falling back to HTTP/1.1 on a second connection, but not once the server has answered (a 103 first: `cancel`); the
  connection's end, a reset and an END_STREAM without a close, each 1006. `test/tls/server.cpp`'s
  `[tls][http][client][http2][websocket]` (wss:// as a stream of the connection a GET made with ALPN h2: echo, the
  close, one connection), `test/tls/client_coalescing.cpp`'s `[websocket]` (with coalescing on, and a.test's connection
  listing b.test's origin, a WebSocket to b.test goes over HTTP/1.1 to b.test's own server; one to a.test on a.test's
  connection), and `test/websocket/handshake.cpp`'s `make_http2_request()` and `check_response()`'s HTTP/2 form.
- Stress: the new tests, 400 times eight at a time, found one race in a test (the scripted server recorded every frame
  once the first had come, and the client's close sometimes came with its message), fixed at its source (it records the
  first only); and one slow test (10 s: a GET whose connection the scripted server never answered was sent again on a
  new connection, as a request dropped before any response is, so the test waited for it), which now stops the GET. Then
  400 out of 400 runs of the new tests passed, eight at a time, and 400 out of 400 of the stage 8 server's
  `[http2][websocket]` and `[http2][tunnel]` tests with the existing `[websocket][client]` ones (61 test cases a run).
- Each new rule and path was broken alone (19 breaks), and the tests failed for every one but three: the pool looking
  only at the host's own connections (the coalescing test), `connect_options::http2`, the setting checked by
  `reserve(true)` (a server not advertising it, and SETTINGS not come), `:protocol` sent, the stream's read timeout off
  (the `io_timeout` test), `response_header_timeout` (hangs, which the run's time limit shows), stop until the head,
  REFUSED_STREAM's fallback, and not after the server's processed it (caught once the 103 test was added for it), the
  key and Upgrade left out of the request, `check_response()`'s HTTP/2 form and its extension check, the END_STREAM
  after the close (`close_sent()` not told, and told but ending nothing: the scripted server waits for it), and
  `http2_tunnel` not done with its stream (the third WebSocket, which needs the stream back, goes over HTTP/1.1). Not
  caught: a tunnel made for a non-2xx too (`check_response()` refuses the response anyway, and the stream's reset either
  way); `stream_tunnel::end()` not marking the stream ended (the session ends it all the same, and resets nothing on a
  stream both sides have closed, so `done()`'s reset sends nothing); `not_opened()` skipped in `open_tunnel()` (only the
  connection's idle bookkeeping and the pool's wakeup, which nothing waits on here; the stop-before-opening test runs
  the path).

## Learned after stage 9: throughput over HTTP/2

`bench_websocket` showed WebSockets over HTTP/2 well behind HTTP/1.1 for large messages (64 KiB one at a time 222 to
263 us against 115 to 119; in flight 165 to 178 against 46), with 84 and 77 allocations per message against 10 and
7.4. Stage 9 guessed at the tunnel's 4 KiB buffers. Measured instead (Release, on this laptop; allocations by a
temporary `operator new` in the bench recording backtraces, symbolized with `atos`; time with `sample`, and with
temporary timestamps at the hand-overs between tasks; all taken out again):

- **The allocations** were the HTTP/2 path's, not the tunnel's. `stream_reader::read()` was always a coroutine, so
  each of the tunnel's 4 KiB reads allocated a frame, even of what was buffered (36 of the 84); `coro::notifier`
  swapped its list of waiters out on each notify, so the next wait allocated a new one (about 16: the stream's
  `changed` and the writer's wakeup); and the connection's writer awaited `io::write_all()`, a coroutine, each round
  (11). What's left (15.6 one at a time, 12.3 in flight, against HTTP/1.1's 10 and 7.4) is a frame per read of the
  stream that waits (`read_waiting()`: a socket's waiting read is an operation, without a frame, which the stream's
  isn't) and, on the client, the write that finds `max_ahead` (64 KiB) in the writer's hands, as a 64 KiB message and
  its header are just over it.
- **The time** was the stream's window. A 64 KiB message's frame (65,546 bytes) is just larger than a stream's
  initial window (65,535), so its last bytes waited for a WINDOW_UPDATE, sent once the reader had read half the
  window: a round trip through both connections' reader and writer tasks, each way, for every message. In flight, the
  window (given back 32 KiB at a time) bounded what was on the way. The allocation fixes alone took one at a time from
  226 to 215 us and in flight not at all; a wider window took them to about 120 and 63. The buffers weren't it: 16 KiB
  tunnel buffers (with the rest of the fixes) measured 126 to 128 us and 67 to 68, slightly worse than 4 KiB's.
- **What's left, one at a time**, is the hand-overs. Timestamps at the stream's write, the writer taking the output, the
  write returning, the reader's read returning and the stream's read resuming, for 16 B messages (51 us a round trip):
  each way, 12 us from the write to the peer's reader (the kernel and the poll, as HTTP/1.1 pays), 5 to 6 us in the
  writer's `send()`, 2.2 us from the stream's write to the writer taking it and 3.3 us from the reader to the stream's
  read resuming (hand-overs through the pool, which HTTP/1.1 doesn't make: the WebSocket's own task reads and writes its
  socket), and 1.4 us in the WebSocket. So four hand-overs a round trip, about 11 us, plus the writer's `poll()` for
  writability each round (`wait_writable()`, with TCP_NOTSENT_LOWAT): what makes HTTP/2's round trip about 20 us
  longer, for small and large messages alike. Removing them means a stream's writer writing the socket itself when the
  connection's writer is idle, or the pool running a task woken by another on the same worker next (Tokio's LIFO slot):
  both change the connection's or the runtime's core, and are left for later. TCP_NOTSENT_LOWAT off made no difference.
- **What's left, in flight**, is CPU: both workers were busy 99% of the time (`sample`: 4,170 samples, 37 idle).
  `send()` 25%, `recv()` 18%, the writer's `poll()` 5%, copying 17% (the session's output, the reader's buffer into
  `pending`, `received`, and out of it: four copies of each message each way, where HTTP/1.1 has none of its own),
  waiting for the connection's lock 8% (the reader holds it while it copies DATA in). Reading straight into the reader's
  buffer (no `pending` copy) measured no difference, nor did reads of 128 KiB rather than 32, nor a tunnel window of
  16 MiB (54 to 62 us), so they weren't kept. Reading a waiting stream's DATA straight into its reader's span (a stream
  read as an operation, as a socket's is) would save a copy and the frame, but touches every path that wakes a stream's
  read; left for later.

The fixes:

- `stream_reader::read()` is a plain function: what's received is taken under the lock and returned as a ready task;
  only a read that has to wait (or a start callback, the server's 100 Continue) calls the coroutine, `read_waiting()`,
  which is the old loop.
- `coro::notifier` holds its first two waiters in place (an array and a count), the rest in the list as before, so a
  wait allocates nothing while two or fewer wait (a stream's reader and writer). Nothing of the notifier is touched once
  a waiter's resumed: the pool's pointer is copied first, as a waiter may destroy the notifier as soon as it runs.
- The connection's writer writes each round with the connection's `write()` (which writes all of it, or fails), not
  `io::write_all()`.
- A tunnel's stream's window is 256 KiB each way (`tunnel_window`): `stream_tunnel`'s constructor calls the session's
  new `widen_window(stream, window)`, which sends one WINDOW_UPDATE for the difference from the initial window, and
  raises the stream's receive window; what's consumed is given back as before (at half the initial window, 32 KiB), so
  the window stays 256 KiB. Its maker wakes the writer (the server did already; the client now does). 256 KiB is a TCP
  receive buffer's order (128 KiB to start on Linux and macOS, growing): 512 KiB measured 56 us in flight and 1 MiB 55,
  against 256 KiB's 63, for twice and four times what a peer that doesn't read can make the other side hold. Requests'
  streams keep 64 KiB.
- Once a tunnel's stream has had all it received read, `take_received()` drops `received`'s memory if it's over
  32 KiB (`tunnel_kept`): a burst no longer stays allocated for the WebSocket's life. (As first written it assigned
  `{}`, an empty initializer_list, which keeps the capacity, so it dropped nothing, and the numbers below were taken
  so. `bench_websocket`'s WebSockets in parallel showed it, and it was fixed after: over HTTP/2, what each WebSocket
  holds after the runs went from about 780 KiB to 140 to 170 with 8 at once, and from 645 to 700 to 130 to 150 with
  64; at the cost of growing the buffer again for each burst: a 64 KiB message one at a time took 129 to 133 us
  rather than 119 to 121, with 22.9 allocations rather than 15.6, and in flight 17.6 rather than 12.3.)
- `io::buffered_reader` and `io::buffered_writer` pass what's larger than the buffer straight through with one read or
  write for all that's left, rather than in pieces of the buffer's size. Over HTTP/2 that's a 64 KiB message written to
  the stream in one write (framed by the session as the windows allow, the caller's span queued as is) and read from it
  as it comes; over HTTP/1.1 one `recv()` and one `send()` rather than four, which made HTTP/1.1's WebSockets faster
  too (one at a time 116 to 98 us, in flight 45 to 38). A read larger than the buffer that meets the end now returns
  what it read with `end_of_stream` (as a socket's does), where reading in pieces had returned it without, the end
  left to the next read; `test/io/buffered_reader.cpp`'s first test changed for it.
- Passing a large write through whole meant `tls::stream::write()` got it whole too, and encrypted all of it at once
  under the stream's lock: a 16 MiB write held about 48 MiB of ciphertext and Botan's buffers, and sent nothing until
  it was all encrypted (a review found it). So `tls::stream` now encrypts and flushes a write larger than 64 KiB a batch
  at a time (`write_batched()`): what a write promises is kept (it returns once all of it has gone, a failure part-way
  is sticky, the socket's limits apply to each batch's flush, a read's replies still go at once, between batches), and
  writes still go whole, in the order they were called: a write that goes in batches takes a turn (`write_turn`, a
  ticket taken when it's called), and a write called while any has a turn takes one and waits for it, as do
  `shutdown()`'s close_notify and `wait_writable()`; a turn ends once, also when its write's task is destroyed before
  it's awaited (skipped when it comes). A small write with no turns waiting goes as before, without a coroutine more.
  `bench_tls writes` (new: the server writes 256 MiB in writes of each size, over loopback, ECDSA, TLS 1.3; three
  runs of each build, interleaved; peak is what the heap held through `operator new` over what it held before, so
  Botan's own secure allocations aren't in it):

  | Write size | Before: us per write | After       | Before: allocations | After  | Before: peak | After   |
  |------------|----------------------|-------------|---------------------|--------|--------------|---------|
  | 16 KiB     | 37.1 to 40.1         | 38.3 to 38.5| 10.8                | 10.8   | 166 KiB      | 166 KiB |
  | 64 KiB     | 144 to 158           | 145 to 154  | 28.3                | 28.3   | 357 KiB      | 357 KiB |
  | 256 KiB    | 584 to 648           | 580 to 619  | 98.3                | 114    | 935 KiB      | 360 KiB |
  | 1 MiB      | 2441 to 2553         | 2364 to 2475| 378                 | 454    | 3.2 MiB      | 357 KiB |
  | 16 MiB     | 47174 to 52007       | 37640 to 39397 | 5978             | 7250   | 48 MiB       | 360 KiB |

  The batch size was measured: 16, 64 and 256 KiB took the same time (about 420 MiB/s, the encryption's pace on this
  laptop), with 1.8, 1.2 and 1.04 times the unbatched allocations for large writes, and peaks of about 140 KiB, 350 KiB
  and 900 KiB over a 16 MiB write: 64 KiB is a few TLS records a batch, and a fifth more allocations than one batch.
  Tests: `test/tls/stream.cpp`'s `[tls][write][batches]` (writes of a batch, just over one, several and odd sizes,
  and 5 MiB, arriving whole; two large writes and a small one, called in order and their tasks run last first, arriving
  in call order, each whole, close_notify after; a write whose task is destroyed unawaited, its turn skipped), and the
  existing slow-peer, stop-token and `wait_writable()` tests, whose 12 MiB writes now go in batches (timing out under
  the write timeout and the deadline, failing the writes after it, and the wait waiting for the write's batches).
  Breaks: a turn's wait not waiting (3 tests fail), the skipped turn not noted, nor stepped past, nor ended by a
  destroyed task (each hangs, which the run's time limit shows), close_notify not taking a turn (2 fail), a small write
  not waiting for turns (2 fail), and `wait_writable()` not taking a turn (its test fails in 4 runs of 6: without the
  turn, the wait comes between the write's batches or during one's flush, by timing). Not caught: no batches at all
  (the data arrives the same; only memory and time show it, which `bench_tls writes` does). Stress: the
  `[tls][write]`, `[tls][wait-writable]`, `[tls][stop]` and slow-peer tests with wss:// over TLS, 400 runs eight at a
  time (13 test cases a run): 400 passed.

What a peer can make the other side hold: a tunnel that isn't read holds up to 256 KiB of what was sent to it (64 KiB
before), in a vector that may be twice that while it's held. The server's connection window still bounds all its
streams together (`http2_max_concurrent_streams` times 64 KiB, at least 1 MiB: 6.4 MiB by default), so a connection
can make the server hold no more than before; it can put more of it in one stream. Writes past the buffers aren't
copied: the stream frames the caller's span as the peer's window allows, and the write returns once it's all framed,
so a peer that doesn't read holds a writer, not memory. Memory per idle WebSocket over HTTP/2: 8 KiB of tunnel buffers
as before, plus at most 32 KiB of `received` (before, whatever the largest burst had grown it to: up to about twice
the 64 KiB window, kept for the stream's life); `coro::notifier` is 24 bytes larger (each stream has one).

Numbers (Release, two workers, best of five runs each; `bench_websocket`'s three runs alike):

| Messages                     | HTTP/1.1, before | after          | HTTP/2, before  | after          |
|------------------------------|------------------|----------------|-----------------|----------------|
| 16 B text, one at a time     | 27.5 to 28.4 us  | 27.4 to 28.2   | 52.3 to 53.8    | 48.5 to 53.8   |
| allocations                  | 4.00             | 4.00           | 12.8 to 13.0    | 6.00           |
| 16 B text, in flight         | 4.1 us           | 4.2 (one 4.7)  | 1.1 to 1.2      | 0.7 to 0.9     |
| allocations                  | 0.37             | 0.37           | 0.13            | 0.01           |
| 64 KiB binary, one at a time | 115 to 118 us    | 96 to 114      | 224 to 243      | 118 to 140     |
| allocations                  | 10.0             | 10.0           | 83.7            | 15.6           |
| 64 KiB binary, in flight     | 44 to 46 us      | 34 to 39       | 161 to 291      | 63 to 77       |
| allocations                  | 7.3              | 7.1            | 77.2            | 12.3           |

(Three runs of each build, interleaved, plus the after build's runs while measuring, which gave 118 to 124 us and 63
to 65 for HTTP/2's large messages; the in-flight row is the noisiest. Along the way, with each fix added: the
allocation fixes alone 215 us and 161 us, 24 and 17 allocations; then the reader and writer going past the buffers 213
and 152, 20 and 16; then the 256 KiB window 118 to 123 and 63 to 65, 15.6 and 12.3. The window without the buffers'
change: 132 to 137 and 72 to 73.)

So large messages over HTTP/2 are now 1.7 to 2 times faster one at a time and 2.5 times in flight, with a fifth of the
allocations, but not as fast as over HTTP/1.1, which the buffers' change made faster too: one at a time about 20 us
slower (the hand-overs, as small messages are), in flight about 1.5 to 2 times (the HTTP/2 path's CPU). Measured
against HTTP/1.1 as it was before (115 to 118 us, 44 to 46), HTTP/2's one at a time is within 5 to 20 us of it, and
in flight 1.4 to 1.7 times it.

`bench_request`'s HTTP/2 rows (Release, the build before and after interleaved, four runs each, best of each; the rows
are bimodal on this laptop, one after-run's 8 bodies at once landing at 2653 us):

| Row                                        | before                   | after                    |
|--------------------------------------------|--------------------------|--------------------------|
| a stream at a time                         | 84.0 us, 49.4 allocations | 81.1 us, 40.9 allocations |
| 8 streams at once (256 KiB each)           | 1234.5 us a round, 97.7 a response | 1181.6 us, 62.3 a response |
| 8 request bodies at once (256 KiB each)    | 1459.6 us a round, 151.4 a request | 1413.9 us, 104.5 a request |

Its HTTP/1.1 rows didn't move (pipelined 1132 to 1147 ns before and after, over a socket 27.6 to 30.9 us both, the
allocations the same).

Tests: `test/http/http2.cpp`'s widened window (the WINDOW_UPDATE's bytes, the peer sending all of it and no more,
given back to it again, and nothing for a window no wider, an unknown stream or one the peer has ended);
`test/http/server_tunnel.cpp`'s `[flow]` (with `test::h2_tunnels`, which gained `wait_send_window()` and
`wait_received()`: the client's window is 256 KiB once the tunnel opens, it can send exactly that while the handler
doesn't read, the handler reads 300,000 bytes in one read, and its write of 1 MiB sends only the client's 65,535 until
the client reads, returning only after); `test/websocket/client_http2.cpp`'s `[flow]` (a scripted server: the client
widens the window after the 200, the server can send exactly 256 KiB of a 300,000-byte message before the client
reads, then all of it; the client's 1 MiB message sends only the server's 65,535 until the server gives the window
back, returning only after, every frame masked and the message intact); `test/coro/notifier.cpp`'s rounds of 1, 2, 3,
5 and 3 waiters and a close of 3; `test/io/`'s large read and write going past the buffer at once.

Break-testing, each alone (14 breaks): caught were the tunnel not widening its window, `widen_window()` not raising
the receive window, not sending the WINDOW_UPDATE, widening a window no wider than the initial one, widening one the
peer has ended, the reader and the writer going back to pieces of the buffer's size, the notifier not resuming the
waiters in place, nor those in the list (both hang, which the run's time limit shows), nor clearing the count (a
coroutine resumed twice: the run aborts), and the ready read not waking the writer for its WINDOW_UPDATE (an existing
test, a handler's reads giving the window back at once). Not caught: the client not waking its writer after widening
(the connection's reader wakes the writer after each receive, which the 200's receive does too, so the WINDOW_UPDATE
goes all the same; the call makes it not depend on that), and dropping `received`'s memory (only memory). The ready
read, the writer's direct write and the notifier's array are only allocations and time: taken out, behaviour is the
same, which `bench_websocket` shows.

Stress: the new and changed tests with `[http2][websocket]`, `[http2][tunnel]`, `[websocket][client]`,
`[http2][server]`, `[http2][client]`, `[session]`, `[flow]`, `[coro][notifier]` and `[io]`'s buffered reader and writer
(308 test cases a run), 400 times eight at a time: 400 passed.

## Stages

Each a pull request, mergeable and tested alone, in this order:

1. **The helpers.** Done (2026-10-24; see Learned in stage 1). `util::sha1`, `util::utf8_validator` (moved from
   Structured Fields, with the ASCII path) and `util::random_bytes` (moved from the DNS resolver). Tests. First: small,
   no I/O, and the next stages need them.
2. **The codec and the session.** Done (2026-10-29; see Learned in stage 2). `websocket/frame.hpp`, `session.hpp`,
   `handshake.hpp` (`accept_key()` and the checks), `websocket/error.hpp`; tests on bytes; `fuzz_websocket`. Without
   I/O, and nothing uses it yet: reviewed and fuzzed on its own, as HPACK and HTTP/2's session were.
3. **Switching an HTTP/1.1 connection.** Done (2026-10-29; see Learned in stage 3). `http/tunnel.hpp`, the switch hook
   (the watch's gate, the reader's hold, the socket's timeouts), `switched()` and the loop's end; tested with a handler
   that switches through `http::detail::switch_protocols()` (internal) and echoes raw bytes, with and without a watch
   begun, over TLS too; `bench_request` before and after. Split from 4 so the server's part, which every request's path
   goes past, is measured and reviewed apart from the protocol.
4. **Accepting WebSockets.** Done (2026-10-29; see Learned in stage 4). `websocket::accept()`, `connection` (reading
   whole and streamed, writing, the write turn, control replies, the close handshake, close codes as errors),
   `max_message`, `example/websocket-echo`, the guide's section; the server tests above but timeouts and `close()`.
   Autobahn's `fuzzingclient` by hand, in the pull request.
5. **Timeouts and the server's close.** Done (2026-10-29; see Learned in stage 5). `idle_timeout` and keep-alive pings, `close_timeout`, `stop`, `close()`'s 1001
   (`tunnel::closing()`, the socket's token); their tests. Split from 4 as they touch the server's lifecycle and are
   tested with time.
6. **The client over HTTP/1.1.** Done (2026-10-29; see Learned in stage 6). `websocket::connect()`, masking, its
   tests, `bench_websocket`; Autobahn's `fuzzingserver` by hand. After the server, so the two test each other.
7. **Extended CONNECT in the session.** Done (2026-10-29; see Learned in stage 7). `:protocol`,
   `settings::enable_connect_protocol`, the rules; tests on bytes and `fuzz_http2`. Nothing advertises it yet: the
   protocol's rules apart from the server, as stage 6's ORIGIN part was.
8. **WebSockets over HTTP/2, the server.** Done (2026-10-29; see Learned in stage 8). `server_config::http2_websockets`,
   `server_request::protocol`, the presentation of extended CONNECT (Decided question 11),
   `http2_stream_ops::open_tunnel`, the stream's interruptible read; `test::h2_client`'s extended CONNECT; browsers by
   hand. Last of the server's: it changes what every HTTP/2 server advertises.
9. **WebSockets over HTTP/2, the client.** Done (2026-10-30; see Learned in stage 9).
   `http2_client_connection::open_tunnel()`, `connect_options::http2`, the pool's check of the peer's setting; tests
   against cppnet's server.
10. **An optional Autobahn workflow** (Decided question 16): a `workflow_dispatch` (and weekly) job on
    `ubuntu-26.04`, outside the matrix and the required `build`, running both suites in Docker against the examples
    and failing on a case that isn't OK or NON-STRICT.

## Decided questions

All decided as recommended (2026-10-22).

1. **Where it lives.** (a) Its own module, `net::websocket` in `websocket/`, depending on `http/`; (b) inside HTTP,
   `net::http::websocket` in `http/`. **Recommended: (a)**: HTTP stays unaware of it but for `tunnel` and
   `:protocol`, and the dependency points one way (as `instrument/`'s middleware does).
2. **SHA-1 for the accept key.** (a) Hand-rolled in `util/sha1.hpp` (about a hundred lines, tested against the RFCs'
   vectors), the same in every build; (b) Botan's (it's in the trimmed build), WebSockets unavailable with
   `NET_TLS=OFF`; (c) Botan's when there, hand-rolled otherwise. **Recommended: (a)**: the handshake's SHA-1 isn't
   security (RFC 6455 uses it only to prove the server read the key), one path is tested everywhere, and the no-TLS
   build keeps WebSockets.
3. **Who owns a server's WebSocket.** (a) The handler's call: the connection stays the server's (slot, `close()`,
   `abort()`, `join()`), and ends when the handler returns; (b) handed over (Go's `Hijack`, hyper's `Upgraded`): the
   application owns the connection, e.g. to keep it in a hub after the handler returns, and the server forgets it.
   **Recommended: (a)**: (b) puts WebSockets outside `max_connections` and graceful shutdown (Go's `Shutdown` doesn't
   track hijacked connections), and a hub can hold a pointer to the `connection` while the handler awaits its close.
4. **The message API.** (a) Whole messages into the caller's buffer and streamed messages as `io::reader`/`io::writer`
   (gorilla's `ReadMessage` on `NextReader`, Beast's `read` and `read_some`); (b) whole messages only (browsers',
   tungstenite's); (c) streamed only. **Recommended: (a)**: whole messages are what most code wants, and streaming lets
   a large message be copied to a file or socket without holding it, at little extra (the same reader underneath).
5. **Concurrency.** (a) One read at a time, writes from any number of tasks, serialized a message at a time (coder's);
   (b) one reader and one writer at a time, concurrent writes a bug (gorilla's, Beast's). **Recommended: (a)**: the turn
   costs little (a notifier, as TLS's flush queue), and a chat-like server writes from several tasks; misuse in (b)
   corrupts the stream silently.
6. **The default message limit.** (a) 32 KiB (coder); (b) 1 MiB; (c) 16 MiB (Beast) or 64 MiB (tungstenite); (d) none
   (gorilla). **Recommended: (b)**: a peer can't make a server hold much per connection, as `max_header_bytes` bounds
   heads; larger messages are an option away, and streaming reads are bounded by it too.
7. **Origin.** (a) gorilla's and coder's default: a request whose `Origin` is present and isn't the request's own host
   (and port) is refused (403), unless it matches `accept_options::origins` (host patterns, `*` for a label), with
   `insecure_any_origin` to skip the check; (b) no check unless asked; (c) a list required. **Recommended: (a)**:
   browsers send cookies with cross-site WebSocket handshakes, so (b) leaves every cookie-authenticated endpoint open to
   cross-site WebSocket hijacking; clients that aren't browsers send no `Origin` and pass.
8. **Keep-alive.** (a) `idle_timeout`, with a ping at half of it, driven by the reading task's transport timeout
   (Beast's behaviour, no task); (b) a task per connection pinging on a timer; (c) none: the application pings (gorilla,
   coder). Default: 60 s, both sides, or off. **Recommended: (a), 60 s**: idle WebSockets behind proxies die silently
   (nginx's `proxy_read_timeout` is 60 s), and a dead peer is found without the application's help; (b) costs a frame
   and a wakeup per connection for what (a) gets from a timeout that exists.
9. **The server's `close()`.** (a) Each WebSocket that's reading sends 1001 (going away) and finishes the close
   handshake within `close_timeout`, so `close()` then `join()` drains WebSockets as it does requests; (b) nothing
   (Go's: the application closes them, e.g. `RegisterOnShutdown`), only `abort()` ends them; (c) handlers get the
   server's closing token (`server_request::closing`) and do as they like. **Recommended: (a)**: what a server going
   down should say (RFC 6455 7.4.1's 1001), and without it `close()` and `join()` wait for WebSockets until `abort()`.
10. **How a close is reported.** (a) As error codes: the peer's close code in `close_category()`, codes this side sent
    at an offset (`from_peer()`, as TLS's alerts), the reason from `close_reason()`; (b) one `errc::closed`, with
    `close_code()` and `close_reason()` accessors. **Recommended: (a)**: the code travels with the error (and compares
    with `status_condition::closed`), as the library reports everything else.
11. **How extended CONNECT reaches handlers.** (a) As it is: method CONNECT, `protocol` "websocket", the target its
    `:path`; a route has to name CONNECT as well as GET (a `WEBSOCKET()` helper registering both); (b) as GET with
    `protocol` set, so a GET route serves both versions; a final 2xx that isn't `accept()`'s (a handler that doesn't
    know WebSockets) is refused (`send()` throws, so the client gets a 500, not a 200 it would take for an open
    WebSocket). **Recommended: (b)**: browsers decide between the versions, not the application, and with (a) a GET
    route answers a browser's extended CONNECT with 405, which fails its WebSocket with no fallback; the guard keeps (b)
    from accepting by accident. `http_metrics` then records GET for both, which is what the request means.
12. **Advertising ENABLE_CONNECT_PROTOCOL.** `server_config::http2_websockets`: (a) on by default; (b) off (Go's).
    **Recommended: (a)**: Go turned it off because its WebSocket packages can't serve extended CONNECT; here the one way
    to accept a WebSocket (`accept()`) serves both versions, so advertising it breaks nothing (with 11 (b)).
13. **The client's API.** (a) `websocket::connect(http::client&, url, options)`: the client's configuration (resolver,
    TLS, timeouts) and, later, its HTTP/2 connections; (b) `websocket::connect(io::scheduler*, url, options)` with its
    own copies of those options (gorilla's `Dialer`, tungstenite's `connect`); (c) both. **Recommended: (a)**: one place
    to configure TLS and resolving, and the pool is where RFC 8441's streams must come from; a program with no other
    HTTP makes a client cheaply.
14. **Redirects in the client's handshake.** (a) Not followed: a 3xx is `bad_status` (browsers, gorilla); (b) followed
    per `max_redirects`, as `send()` does. **Recommended: (a)**: the WHATWG standard fails them, so servers don't rely
    on them, and credentials across a redirect would need `send()`'s rules again.
15. **permessage-deflate.** (a) Out of scope: offers declined, RSV1 refused; (b) a later stage with a hand-rolled
    DEFLATE (inflate is a Huffman decoder much like HPACK's, a few hundred lines; deflate can start with stored and
    fixed blocks, then LZ77 matching); (c) zlib as an optional dependency (a vcpkg feature, as `tls` brings Botan).
    **Recommended: (a)** now, with (b) as its own plan if a use appears: browsers work without it, coder ships it off
    and warns against it unmeasured, gorilla's is experimental, and it brings memory per connection (a 32 KiB window
    each way with context takeover) and CRIME-like risks for secrets mixed with attacker-chosen text.
16. **Autobahn.** (a) By hand only, where Docker is; (b) that, plus an optional workflow (`workflow_dispatch` and
    weekly) on GitHub's Linux runner, which has Docker, outside the required `build`; (c) in every CI run.
    **Recommended: (b)**: the suite is the reference for conformance, and the workflow costs nothing locally (Docker is
    available locally, but the macOS runners have none); (c) would put a Docker job and minutes of cases on every push,
    and the table-driven tests cover its cases' rules between runs.

## Out of scope

- permessage-deflate and other extensions (Decided question 15).
- WebSockets over HTTP/3 (RFC 9220), with `http3.md`.
- Other upgrades and tunnels: `h2c` upgrade, CONNECT proxying (plain CONNECT stays refused as a target), `:protocol`
  values but `websocket` (CONNECT-UDP, WebTransport).
- A hub or broadcast layer, JSON messages, reconnection: the application's, on top of `connection`.
- Proxies for the client (HTTP CONNECT, SOCKS): the HTTP client has none either.
- Handing a server's connection over to the application (Decided question 3 (b)).
