# Plan: fixing what the TLS and HTTP/2 reviews found

Status: planned (2026-10-18); groups 1 and 2 done. Six groups, each its own branch and pull request, in order. Each fix comes with a test
that fails without it. Findings are labelled as the reviews' were: TLS (T), the HTTP/2 protocol (P), server (S),
client (C), and the effects on what existed before (R).

## 1. HTTP/2 flow control and buffers

- **Unread data leaks the connection's window (C1, S1).** `http2_connection_base::remove_stream()` is the one place a
  stream's leftover data is handed back: it calls `session.consume(id, received.size() - received_start)` for what's
  buffered but unread, and drops the buffer. The server's `run_stream` and the client's `done()` both go through it.
  (The session part already handles streams it has dropped: it credits only the connection's window.)
- **The window after a stream error (P7).** When `on_data` has taken a frame's length from the connection's window,
  then fails the stream (its flow control, or a Content-Length mismatch), it gives the length back at once, as its
  unknown-stream path does.
- **Many large responses read one at a time deadlock (C2).** The connection's window is never smaller than the
  streams' windows combined could need: the client's defaults to 1 GiB (as Go's), the server's to
  `max_concurrent_streams * initial_window_size`, at least 1 MiB. Memory stays bounded by the streams' windows (each
  can only have its own outstanding). Both stay configurable through `session_config`.
- **A slow reader's buffer keeps the whole body (S2).** `stream_reader::read` erases the consumed prefix once it's at
  least half the buffer, so the buffer is bounded by about twice the stream's window.
- **WINDOW_UPDATE waits for the reader to block (S7).** `stream_reader::read` calls `wake_writer()` after
  `consume()`, once it has released the lock.

Tests: the server with 30 POSTs of 60 KB to a route that ignores its body, then an upload that must complete; the
client dropping 30 partly read responses, then reading a whole one; 24 concurrent 4 MB responses read one at a time;
a slow reader with PINGs flowing, whose buffer stays bounded.

## 2. HTTP/2 streams' lifecycle, timeouts and limits

- **A client stream that fails before its head is never removed (C3).** `http2_client_connection::send()` arms a
  `util::defer` once the stream's open, which calls `done(id)` (reset if unfinished, remove, wake the pool) unless
  the response's body took the stream: so every failure (refused, reset, timeout, body error, the awaiting coroutine
  destroyed) removes it. The client also overrides `after_receive` to call `on.freed()` whenever a receive ended a
  stream, so a strict waiter learns of the free stream when the session does, not when the response is destroyed.
- **Timeouts restart on any wakeup (S4, C15).** A deadline means "no progress since": it's set when a wait starts only
  if none is set, and cleared only by progress, in the events (`on_data` clears the read deadline; a stream's window
  growing, the write deadline; `on_headers`, the head's). That also closes the race of the timer resetting a stream
  whose head has just arrived.
- **One deadline for a stream's reader and writer (S9).** `read_deadline` and `write_deadline`, both checked by
  `check_streams`.
- **A writer held by the 1 MiB output cap has no deadline (S8).** It sets its write deadline too, and a stream's reset
  or timeout notifies `drained` as well as `changed`.
- **Handlers aren't bounded (P3, S5).** As Go (its fix for CVE-2023-44487): the server counts handlers started but not
  finished; at `max_concurrent_streams`, a new stream is queued, its handler started when another finishes; past
  `4 * max_concurrent_streams` running and queued, the connection ends with GOAWAY(ENHANCE_YOUR_CALM).
- **A malformed PRIORITY resets a stream silently (P2, P10).** `on_priority` takes the events, so its stream error
  calls `on_stream_reset`; on a stream that isn't open (idle or closed) it sends no RST_STREAM (which RFC 9113 forbids
  on an idle stream), and only counts as a glitch.
- **RST_STREAM(NO_ERROR) doesn't end the stream (C5).** If the stream's incoming side has ended, NO_ERROR means "stop
  sending" (RFC 9113 section 8.1): only sending stops, and the response stands. Otherwise the stream fails with
  `std::errc::connection_reset` (which `status_condition::closed` matches), as a code of value 0 can't be a failure.
- **A handler that throws mid-body sends a truncated response that looks complete (S3).** `run_stream` decides before
  finishing: if the handler threw after its head was sent, or wrote less than its Content-Length, the stream is reset
  (INTERNAL_ERROR) instead of finished, while the stream still exists, so the reset reaches the client.
- **After a throw (S10).** The 500 clears the headers the handler set (as HTTP/1.1's `respond_and_close`); whether the
  final response was sent is `sent()`, not `status_code() == NONE` (so a 1xx then a throw still gets its 500);
  `refuse()`'s 400 and 501 get a Date.
- **A refusal's RST waits (S12).** `after_receive` wakes the writer after refusing a stream it couldn't spawn.
- **The notifier resumes inline when the pool refuses (S11).** Check that the scheduler's ownership order rules it out,
  then make that fallback log and terminate rather than deadlock silently.

Tests: a request failing before its head (refused, header timeout), then the idle timeout firing, and a strict waiter
woken; WINDOW_UPDATE(0) every 0.5 s not holding off `body_read_timeout`; open-then-reset loops against the handler
limit (queueing, then ENHANCE_YOUR_CALM); PRIORITY on an open stream; RST(NO_ERROR) before and after the response's
end; a handler throwing mid-body (the client sees a reset); a 1xx then a throw. `h2_client::pump` gets a deadline, so
a server bug fails a test rather than hanging it.

## 3. HTTP/2 protocol validation

- **Content-Length isn't checked when trailers end the stream (P1).** `end_header_block` checks
  `received == content_length` for trailers that end the stream (unless HEAD, 204 or 304), as `on_data` does for DATA
  with END_STREAM: a stream error (PROTOCOL_ERROR) if not.
- **GOAWAY's last stream can rise (P4).** `connection_error`'s GOAWAY uses `min(highest_peer_stream, local_goaway)`,
  as `goaway()` does.
- **Frames on streams ignored after this side's GOAWAY are errors (P5).** Streams above the last stream sent are
  ignored, frame by frame: DATA gives its length back to the connection, HEADERS is decoded (to keep HPACK in step) and
  dropped, neither counted as a glitch. HEADERS on a stream too old to remember (its reset fell out of the 128
  remembered) is a stream error (STREAM_CLOSED) and a glitch, rather than a connection error.
- **Host (P6).** `scan_fields` counts `host` fields (more than one is malformed), and compares it with `:authority`
  ignoring ASCII case.
- **HEADERS depending on itself (P8).** HEADERS with the PRIORITY flag is checked for it (a stream error,
  PROTOCOL_ERROR), as the PRIORITY frame is.
- **A header block over the limit (P9).** Ending the connection is right (the block must be decoded, to keep HPACK's
  table in step); CLAUDE.md and the HTTP/2 plan say so. `max_continuations` becomes the larger of 8 and enough frames
  for `max_header_list_size` at the peer's frame size.
- **Controls in values (P11).** `valid_value` also rejects 0x01 to 0x1f (but tab) and 0x7f, as HTTP/1.1 does.
- **Response fields aren't checked (S6).** `append_fields` validates names and values with HTTP/1.1's encoder's check
  (shared), throwing as it does.
- **Fields named by Connection (C17).** `append_fields` drops them too (RFC 9113 section 8.2.2).
- **Fuzzing.** `fuzz_http2`'s client opens streams before receiving, its server submits, resets and consumes, and
  `now` advances with the input; `fuzz_hpack` decodes several blocks per input through one decoder, with size updates
  between them. hpack-test-case's vectors are a test, as the HTTP/2 plan intended.

Tests: a session test for each of P1, P4, P5, P6, P8 and P11; a response field with a line break throwing over
HTTP/2.

## 4. The client and its pool

- **Idle HTTP/2 connections can't be evicted (C9, R2).** `evict_idle` also considers HTTP/2 connections with no
  streams and none reserved, by how long they've been idle. Evicting one counts it out of `open` at once (a flag on its
  entry makes its `closed` callback skip `forget()`), so there's room to connect at once.
- **Destroying the client cuts off bodies being read (C4).** The pool's destructor closes only idle HTTP/2
  connections; the others get GOAWAY and end once their last stream is done (`maybe_end`), kept alive by their tasks
  and responses.
- **Request bodies ignore Content-Length (C6).** `send_body` sends exactly `Content-Length` bytes when it's set
  (through a `limit_reader`), failing with `invalid_argument` if the body is shorter, as HTTP/1.1's `copy_length`.
- **The caller's Host is dropped (C7, R3).** A caller's `Host` becomes `:authority` (RFC 9113 section 8.3.1), and
  isn't sent as a field; otherwise `:authority` comes from the URI, without the scheme's default port, as HTTP/1.1's
  `Host`.
- **Strict waits behind a draining connection (C8).** Only connections that can still open streams (not ending, no
  GOAWAY from the server) make a strict request wait.
- **A blocked write is never noticed (C10).** The pool sets the socket's write timeout to `io_timeout` on HTTP/2
  connections (as the server sets `write_timeout`): a writer stuck past it fails the connection, which fails its
  streams.
- **Refused before anything was sent (C11).** `send()` reports a request refused before it sent anything (`open()`
  failing, or the connection ending first), which is retried even with a body (it wasn't read). `idle()` and the idle
  timeout count reserved streams.
- **`idle_timeout = 0` (C12)** means keeping nothing idle for HTTP/2 too: the connection closes when its last stream
  is done.
- **A refused request is retried on the same connection (C13).** The retry passes the connection that refused it to
  `acquire`, which skips it.
- **A burst opens a connection each (C14).** The pool remembers which hosts agreed on HTTP/2; for those, while a
  connection is being made, other requests wait for it (on `released`, within `connect_timeout`) rather than each
  connecting. (The very first burst to a host still connects in parallel: nothing can know it speaks HTTP/2 until its
  first handshake.)
- **A body reader that returns nothing spins (C16).** `{0, no error}` fails with `io::errc::no_progress`, as
  HTTP/1.1's.
- **HTTP/2 by default (R4).** A request without a body whose connection drops (without GOAWAY) before any head is
  retried once on another connection if it's idempotent: HTTP/1.1's `closed_early` retry's counterpart. HTTP/1.0 and
  `Connection: close` mean nothing over HTTP/2: documented, with `client_options::http2 = false` as the way to force
  HTTP/1.1.

Tests: GOAWAY and REFUSED retries (the latter on another connection); `response_header_timeout` and `io_timeout`;
the client destroyed mid-body; eviction at `max_connections`; a caller's Host; Content-Length longer and shorter than
the body; strict while a connection drains after GOAWAY; a burst to a known HTTP/2 host; `idle_timeout = 0`. The
dropped-response test checks for the RST and that the connection is the same one; the idle timeout test checks while
the scheduler runs.

## 5. HTTP/1.1, CI, formatting, and HTTP/2's costs

- **An idle cleartext HTTP/1.1 connection is held for twice `header_read_timeout` (R1).** `serve_requests` computes
  the first head's deadline once, before the preface check, and the first decode reuses it.
- **`max_header_bytes` near SIZE_MAX wraps (R5).** `http2_session_config` adds with saturation.
- **A stale comment (R8)** at `server.cpp:475`, rewritten.
- **cmake-format damaged comments (R7).** `ports/botan/portfile.cmake` is restored to its form before `db31961`
  (upstream vcpkg's), and `ports/` excluded from cmake-format (the CI check, and CLAUDE.md); `fuzz/CMakeLists.txt`'s
  command example goes between `# cmake-format: off` and `# cmake-format: on`.
- **CI (R6).** Changed headers are linted too (clang-tidy infers a header's command from the compile database); a push
  whose `before` is all zeros, or not in the clone, lints against `HEAD^`; the format job installs clang-format from
  PyPI pinned to major 23 (`clang-format==23.*`, in the same virtualenv as cmakelang), and CLAUDE.md says to use that
  locally, so a new major is taken deliberately, with a commit that reformats.
- **Costs (R9).** `starts_with_preface` checks what's buffered without a coroutine, waiting through one only when it
  must (`ready_task`); a stream ending wakes the pool's waiters only if any wait (a waiter count). The client's extra
  `send_http11` frame (an allocation per request, against a network round trip) stays.
- **Public headers include HTTP/2's internals (R10).** `session_config`, `settings` and `token_bucket` move to
  `http/http2_config.hpp`; `connection_pool.hpp` and `server.hpp` include that and forward-declare the connections.

- **A connection that ends with streams in flight isn't closed until their handlers return (found after group 2).**
  After a connection error's GOAWAY (e.g. ENHANCE_YOUR_CALM at the handler limit), a protocol error, a failed write, or
  the client closing, `http2_connection_base::run()` waits for the handlers before the connection is closed: RFC 9113
  section 5.4.1 says the TCP connection MUST be closed, and a client waits for the streams GOAWAY says may have been
  processed until it is (or for ever). `run()` shuts the connection down (`detail::shutdown()`: close_notify over TLS,
  then FIN) once the writer has flushed what's left (after `workers.join()`), and only then awaits `finish_streams()`.
  The fd, and the server's `max_connections` slot, are still held until the handlers return, deliberately: as handlers
  can't be cancelled, that bounds those left running to `max_connections * max_concurrent_streams` (freeing the slot
  would let a client reconnect and pile up more); HTTP/1.1 holds its slot the same way. The comments on `run()` and
  `schedule()`, and `docs/plans/http2.md`, say so. Graceful `close()` is unchanged.
- **The calm GOAWAY's last stream over-claims.** It's the highest stream whose handler was started, not
  `last_peer_stream()` (which counts queued streams that never start, and the one that tripped the limit), so the
  client may retry the rest.
- **The stream that trips the handler limit stays in the stream maps.** It's removed (`remove_stream()`, which gives
  back its unread data) when the connection is ended for it.

Tests: a cleartext connection that sends nothing closes at `header_read_timeout`; `max_header_bytes = SIZE_MAX`; at
the handler limit, the client sees the connection close while the handlers are still held, and (with
`max_connections = 1`) a second client's request is answered only once they've finished; a client that closes its side
with a handler held gets the server's FIN at once; the calm GOAWAY's last stream is the last started.

## 6. TLS

- **The client reuses a connection holding decrypted bytes not yet read (T1).** `tls::stream` gets an internal
  `idle_clean()`: nothing received and unread, no close_notify from the peer, no failure, and no partial record inside
  Botan (`received_data()`'s return, how much more the next record needs, is kept). `still_open()` checks it for TLS
  connections, as well as peeking at the socket. `docs/plans/tls.md`'s "works for TLS too" is corrected.
- **`flush()` returns while another task is still flushing (T2).** Flushing is a queue: a caller that finds a flush in
  progress waits for it to finish (a `coro::notifier`, from the socket's scheduler's pool), then flushes what's still
  queued, so a write returns only once its ciphertext has gone to the socket. Backpressure, deadlines and timeouts
  apply to every writer, and `shutdown()`/`close()` wait for what's queued and close_notify.
- **A failed write isn't kept (T4).** It's recorded as the stream's write failure, which every later write and flush
  returns.
- **An empty read blocks (T3).** It returns `{0, no error}` at once.
- **A key that doesn't match the certificate is accepted (T6).** `server_context::make()` compares the key's public
  key with the certificate's (their encodings), failing if they differ.
- **A test's writer can outlive its stream (T7).** "a client and server talk over TLS" waits for its writer task.
- **Docs (T8).** The handshake timeout's comment (`server.cpp:395`, and `tls.md`): `body_read_timeout` limits each
  wait; `docs/guide.md:241`: `client_options::tls.insecure_skip_verification`; `tls.hpp:103-104` holds once T2 is
  fixed.

Tests: a response injected after a body (the review's reproduction), and close_notify arriving with a body's end;
HTTP/1.1 over TLS (the client with `http2` off); a truncated body without Content-Length over https; an idle
connection closed with close_notify, then the retry; a fatal alert mid-stream; the empty host guard; the mismatched
key; an empty read; a slow peer while two tasks write.

## Afterwards: cancelling handlers

Handlers can't be cancelled: `server_request` has no stop token, so a handler that does no I/O (a sleep, a database
call, a long computation) doesn't learn that its stream was reset or its connection ended, which is what makes a
connection's `max_connections` slot (see group 5) held long. Once group 6 is done, plan a `std::stop_token` on
`server_request` (stopped when the stream fails: a reset, a timeout, the connection ending; and for HTTP/1.1 when the
connection ends), and how handlers and middleware would use it.
