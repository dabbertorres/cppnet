# Plan: WebSockets

Status: decided (2026-10-22): every question at the end as recommended. Stage 1, the helpers, done (2026-10-24; see
Learned in stage 1). Stage 2, the codec and the session, done (2026-10-29; see Learned in stage 2). Stage 3, switching
an HTTP/1.1 connection, done (2026-10-29; see Learned in stage 3). Stage 4, accepting WebSockets, done (2026-10-29; see
Learned in stage 4).

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

Over HTTP/2 (a later stage): when the client's pool has an HTTP/2 connection to the host whose peer sent
ENABLE_CONNECT_PROTOCOL = 1 (and `connect_options::http2`, off by default at first), `connect()` opens an extended
CONNECT stream on it instead (`http2_client_connection::open_tunnel(request)`, internal: the request head without
END_STREAM, `:protocol`, the stream's `read_timeout` off, its reader and `stream_writer` as the tunnel, a reset on
`abort()`); otherwise as above. RFC 8441 forbids extended CONNECT before the setting arrives, so a connection still
waiting for the server's first SETTINGS isn't used.

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
  two 4 KiB buffers. The client: the connection, two 16 KiB buffers, and the state.
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
5. **Timeouts and the server's close.** `idle_timeout` and keep-alive pings, `close_timeout`, `stop`, `close()`'s 1001
   (`tunnel::closing()`, the socket's token); their tests. Split from 4 as they touch the server's lifecycle and are
   tested with time.
6. **The client over HTTP/1.1.** `websocket::connect()`, masking, its tests, `bench_websocket`; Autobahn's
   `fuzzingserver` by hand. After the server, so the two test each other.
7. **Extended CONNECT in the session.** `:protocol`, `settings::enable_connect_protocol`, the rules; tests on bytes and
   `fuzz_http2`. Nothing advertises it yet: the protocol's rules apart from the server, as stage 6's ORIGIN part was.
8. **WebSockets over HTTP/2, the server.** `server_config::http2_websockets`, `server_request::protocol`, the
   presentation of extended CONNECT (Decided question 11), `http2_stream_ops::open_tunnel`, the stream's interruptible
   read; `test::h2_client`'s extended CONNECT; browsers by hand. Last of the server's: it changes what every HTTP/2
   server advertises.
9. **WebSockets over HTTP/2, the client.** `http2_client_connection::open_tunnel()`, `connect_options::http2`, the
   pool's check of the peer's setting; tests against cppnet's server.
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
