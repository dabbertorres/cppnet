# Using cppnet

This is how the library fits together: the runtime every part of it runs on, how errors are reported, what has to
outlive what, and then each part. The headers' comments have the details, and [example/](../example/) has whole
programs. Everything is in `namespace net`, with a namespace per directory (`net::io`, `net::http`, ...), and headers
are included from `include/`: e.g. `#include "http/server.hpp"`.

## The runtime

All I/O is done by coroutines, which wait for it without blocking a thread. Three things run them:

- **`coro::thread_pool`** (`coro/thread_pool.hpp`): worker threads that run coroutines. Make one, sized to the machine
  by default, and give it to a scheduler.
- **`io::scheduler`** (`io/scheduler.hpp`): runs tasks on the pool, and resumes those waiting for I/O, a timeout, or
  each other. Most objects that do I/O (sockets, listeners, servers, clients) take a `scheduler*`.
- **`coro::task<T>`** (`coro/task.hpp`): what a coroutine returns. It's lazy: it starts when it's `co_await`ed (or
  spawned), and `co_await` gives its result.

```cpp
net::io::scheduler scheduler{std::make_shared<net::coro::thread_pool>()};

// a coroutine: returns a task, and co_awaits other tasks.
net::coro::task<int> answer(net::io::scheduler* scheduler)
{
    co_await scheduler->sleep_until(net::io::clock::now() + std::chrono::milliseconds{10});
    co_return 42;
}
```

There are two ways to run tasks:

- **`scheduler.run_to_completion(task)`** runs one task to its end, blocking the calling thread, and returns its
  result. Then it stops the scheduler. It suits a program that does one thing (e.g. `example/http-get`), and tests.
- **`scheduler.spawn(task)`** starts a `task<>` in the background, owned by the scheduler until it finishes. A server
  is spawned this way (`scheduler.spawn(server.serve())`). Spawning returns false (and drops the task) once the
  scheduler is stopping.

The pool's idle workers wait for I/O themselves, so nothing else needs to run: a server's main thread can just wait
for a signal (see `example/http-server`). `scheduler.run()` exists for running the scheduler on a thread of your
own, but isn't needed.

To stop:

- **`scheduler.request_stop()`** refuses new tasks, and cancels what tasks are waiting for (their waits fail with
  `std::errc::operation_canceled`), so they finish by themselves. It may be called from anywhere, including a task.
  Then it stops `scheduler.stop_token()`, for work that should end with the scheduler but isn't waiting on it.
- **`scheduler.shutdown()`** also waits for the tasks to finish. It's called by the destructor. It can't be called
  from one of the scheduler's own tasks, which would wait for itself: it throws `std::logic_error` there (and the
  destructor terminates).

A server's `close()` stops it accepting and ends its connections gracefully (those waiting for a request at once, the
others once their requests are done), so a server is stopped first: `server.close()`, then `co_await server.join()`
for the requests in progress to finish (for a while: then `server.abort()` stops them), then `scheduler.shutdown()`.
`request_stop()` aborts the servers using the scheduler too, as well as canceling what their requests are waiting for
(see HTTP server).

One wait can be canceled alone, with a `std::stop_token`: the scheduler's waits take one as a last argument
(`sleep_until(deadline, stop)`, `wait_on(word, expected, deadline, stop)`, `wait(fd, op, stop)`, ...), and end with
`std::errc::operation_canceled` when stop is requested (at once, if it already was), unless they've ended already.
Sockets take one with `set_stop_token()` (see TCP). A token that can't be stopped (e.g. a default-constructed one) is
as none. The callback that cancels a wait takes the scheduler's lock for it, and runs on whoever requests stop. An
HTTP server's handler gets one with its request (`req.stop`), stopped once the request is no longer wanted, and an
HTTP client's request takes one (`client_request::stop`).

```cpp
// sleeps for a minute, unless stop is requested first.
auto err = co_await scheduler->sleep_until(net::io::clock::now() + std::chrono::minutes{1}, stop);
if (err == std::errc::operation_canceled) { /* stopped */ }
```

A token of your own that the request's (or any other) also stops is a `util::linked_stop_source{parent}`
(`util/linked_stop_source.hpp`): a `std::stop_source` with a `std::stop_callback` on `parent`, so it's stopped when
`parent` is, or by its own `request_stop()` (which doesn't stop `parent`). It can't be moved, as the callback refers to
it. As with any stop callback, its own callbacks then run on whichever thread stopped `parent`.

`scheduler.call_at(deadline, func)` calls a `noexcept` function at a deadline, rather than resuming a task: e.g. to
request stop on a source, with no task waiting to do it. It returns the call, which is the caller's (nothing is
allocated) and can't be moved: `cancel()` (or destroying it) cancels it, waiting for `func` to return if it's being
called on another thread, so what `func` refers to may go once it has. `func` runs on the thread polling for the
scheduler (a worker), holding none of the scheduler's locks, so it may request stop on a source whose tokens waits
were given; it must be quick, and not block. A call isn't made once the scheduler is stopped.

```cpp
net::util::linked_stop_source source{req.stop};
auto deadline = scheduler->call_at(net::io::clock::now() + 5s, [&source] noexcept { source.request_stop(); });
co_await work(source.get_token());   // stopped after 5 seconds, or with the request
```

Tasks shouldn't block (e.g. on a mutex held for long, or a blocking syscall), as that holds up a worker:
`scheduler.run_blocking(func)` runs a blocking function on threads set aside for that (4, unless the scheduler is made
with `io::scheduler::options{.blocking_threads = n}`; the rest wait for one), and returns a task of its result.
`run_blocking(func, stop)` doesn't run `func` if stop was requested before it starts (its result, a `std::expected`, is
then `operation_canceled`), and passes the token to a `func` that takes one; nothing interrupts `func` once it has
started, and `shutdown()` waits for it, as for any task.

## Lifetimes

As tasks are lazy, a coroutine's reference (and view) parameters must outlive its task, not only the call. Awaiting
the call straight away is always fine, as temporaries last until the end of the full expression, which includes the
`co_await`:

```cpp
co_await net::io::write_all(*body, std::format("{}\n", n));   // fine: the string lasts until the write is done.

auto writing = net::io::write_all(*body, std::format("{}\n", n));
co_await std::move(writing);                                    // wrong: the string was destroyed before the write.
```

Functions that keep what they're given take it by value (e.g. `http::client::send(client_request)`), and so do
`thread_pool::schedule()`'s arguments.

The objects that coroutines use must outlive them too:

- the scheduler outlives everything that uses it;
- a socket outlives the reads and writes waiting on it;
- a client outlives its requests (copies of a client share its connections, so a copy may be what a task keeps);
- a server's destructor waits for the task `serve()` returned (from when it's called: to finish, or to be destroyed
  unrun, as by a scheduler that refuses it) and for its connections (so it can't be destroyed from one of the
  scheduler's tasks); `join()`, which waits for them in a task, must start while the server exists.

## Errors

Failures are reported, not thrown: functions that can fail return `std::expected<T, std::error_code>`, and reads and
writes return an `io::result{count, err}`. Errors are `std::error_code`s: the OS's (`std::system_category()`), or the
library's own (`io::errc`, `tls::errc`, `dns::errc`, `gai_category()`'s, ...).

Compare an error against a condition, which matches every error that means the same, rather than a particular code:

```cpp
auto res = co_await socket.read(buf);
if (res.err == std::errc::timed_out) { /* the read's timeout or deadline passed */ }
if (res.err == net::io::status_condition::closed) { /* the stream ended: cleanly, or reset */ }
if (res.err == net::io::errc::end_of_stream) { /* only the clean end of the stream */ }
```

- The end of a stream is `io::errc::end_of_stream`, possibly with the last bytes read. `io::status_condition::closed`
  matches that, the connection being reset or aborted, and TLS's `stream_truncated`.
- Timeouts are `std::errc::timed_out`, and stopping the scheduler (or a stop token given to a wait, a socket or a
  request) `std::errc::operation_canceled`.
- A name that doesn't resolve compares equal to `dns::condition::not_found` (and a failure that might pass to
  `dns::condition::temporary_failure`), whichever resolver reported it (see DNS).
- `error.message()` says what happened, for logs.

Exceptions are for programming errors (e.g. a second final response to a request, or an invalid metric name), and
the few constructors that can fail: e.g. a `listener` or a `server` constructed with a port that's taken throws
`std::system_error`, where `listener::make()` returns the error.

## Reading and writing

`io::reader` and `io::writer` (`io/reader.hpp`, `io/writer.hpp`) are the interfaces for streams of bytes, which
sockets, TLS streams, HTTP bodies, and in-memory buffers (`io::string_reader`, `io::string_writer`) implement:

- A **read** fills some of a buffer (at least a byte, unless it fails), and returns how much.
- A **write** writes some of the data. `io::write_all(writer, data...)` writes all of it.
- **`io::read_all(reader, max_size)`** (`io/util.hpp`) reads to the end of the stream, into a `std::string`.
- `io::buffered_reader` and `io::buffered_writer` buffer another reader or writer; `io::readline` reads lines.

## TCP

```cpp
// a server's side: listener::make binds and listens (port "0" lets the OS choose).
auto listener = net::listener::make(&scheduler, "127.0.0.1", "7777");
if (!listener) { /* e.g. listener.error() == std::errc::address_in_use */ }

auto conn = co_await listener->accept();          // std::expected<tcp_socket, std::error_code>

// a client's side: resolves the host, and connects, within connect_options' timeout (10s), until its stop token (if
// any) is stopped. The port is a number.
auto sock = co_await net::tcp_socket::connect(&scheduler, "example.com", "80");

// or connects to addresses, in order, without resolving anything.
const std::array<net::ip_addr, 2> addresses{net::ipv6_addr::loopback(), net::ipv4_addr::loopback()};
auto local = co_await net::tcp_socket::connect(&scheduler, addresses, 7777);
```

`connect` tries a host's addresses in order, until one accepts. Each attempt gets a share of the timeout, as Go's
dialer gives it: what's left over the addresses left, but at least 2 seconds (never past the end), so an address that
doesn't answer leaves time for the rest. A name is resolved first, with `connect_options::resolver` (by default
`dns::default_resolver()`, see DNS), within the same timeout, and stop token; an IP address (e.g. `"127.0.0.1"` or
`"::1"`, or `"fe80::1%en0"` with its zone) isn't, so it's connected to without a thread. A port that isn't a number
(e.g. `"http"`) fails with `std::errc::invalid_argument`. `connect_options::proto` limits it to IPv4's or IPv6's
addresses. It fails with the resolver's error, the last address's (e.g. `std::errc::connection_refused`), `timed_out`,
or `operation_canceled`.

A socket's reads and writes can be limited by a **deadline** (a time by which they must finish:
`set_read_deadline()`, `set_write_deadline()`, `set_deadline()`) or a **timeout** (how long each may go without
progress: `set_read_timeout()`, ...), and fail with `std::errc::timed_out` if it passes. `set_stop_token(stop)` makes
them fail with `std::errc::operation_canceled` when stop is requested; as with a deadline, only reads and writes that
have to wait for the socket are affected. A read canceled while it waits has read nothing, so the socket can be read
again; a canceled write's result counts what it wrote. `shutdown_write()` ends the outgoing side (the peer reads the
end of the stream).

## DNS

```cpp
// a name's addresses: std::expected<std::vector<ip_addr>, std::error_code>.
auto addrs = co_await net::dns::default_resolver().resolve(
    &scheduler, "example.com", {.deadline = net::io::clock::now() + std::chrono::seconds{5}, .stop = stop});
if (!addrs && addrs.error() == net::dns::condition::not_found) { /* no such name, or no addresses */ }
```

A `dns::resolver` (`dns/resolver.hpp`) looks names up with the system's resolver (`getaddrinfo()`, on the scheduler's
threads for blocking calls) or its own (the native back end, below), choosing per name as Go does (`backend::automatic`,
below); `dns::default_resolver()` is the one `connect` and the HTTP client use unless given another.
`resolve_options::proto` limits it to IPv4's or IPv6's addresses. An IP address is returned as it is, without a thread
or a lookup, and a name that isn't valid (labels of letters, digits, `-` and `_`) fails with
`std::errc::invalid_argument` without one. Concurrent calls for the same name (ignoring case) and families, on the same
scheduler, share one lookup (as Go's `singleflight` merges them), so a burst of connections to a host resolves it once;
nothing is cached, so a call after it has finished looks the name up again. A caller gives up at its deadline
(`timed_out`) or when stop is requested (`operation_canceled`, as when the scheduler is stopped), at once, and the
callers sharing its lookup go on waiting. Once they've all given up, the lookup is stopped, and a later call starts
another rather than share it: `getaddrinfo()` can't be interrupted, so it goes on holding its thread until it returns,
and its result is dropped (a lookup whose callers gave up before it got a thread isn't started), and the scheduler's
`shutdown()` waits for it. The system's errors are `getaddrinfo()`'s (in `gai_category()`), the native's `dns::errc`'s
(below): compare either with `dns::condition`.

```cpp
// the native back end, with its own configuration (without one, it reads /etc/resolv.conf, /etc/hosts and
// /etc/nsswitch.conf, and again when they change, at most every 5 seconds).
net::dns::config conf = net::dns::parse_resolv_conf("nameserver 192.0.2.53\noptions timeout:2 attempts:3\n");
conf.hosts            = net::dns::parse_hosts("192.0.2.10 build.internal\n");

const net::dns::resolver resolver{{.use = net::dns::resolver::backend::native, .conf = std::move(conf)}};
auto addrs = co_await resolver.resolve(&scheduler, "example.com", {.stop = stop});
```

`backend::native` chooses the resolver's own back end, as Go's: no thread, but DNS queries it sends itself; and
`backend::system` chooses the system's, whatever `automatic` would. It works from a `dns::config` (`dns/config.hpp`,
which `dns/resolver.hpp` includes): the nameservers (addresses and ports), the search list and `ndots`, the timeout per
attempt and the attempts, `rotate`, `single-request`, `single-request-reopen`, `use-vc`, `trust-ad`, the hosts table,
and whether the hosts table or DNS comes first (nsswitch.conf's `hosts:` line, as far as `files` and `dns` go).
`parse_resolv_conf()`, `parse_hosts()` and `parse_nsswitch_hosts()`/`order_of()` read the files' text as Go does (a
resolv.conf without nameservers means 127.0.0.1 and ::1). A name is looked up in the hosts table, then, under each name
the search list makes, both families' queries go out at once on one UDP socket (a new one per attempt, so a new random
source port) with random IDs and EDNS(0) (offering 1232 bytes; asked again without it if a server answers FORMERR), and
replies that don't match a query (ID, question) are ignored. A truncated answer is asked again over TCP, a server that
fails (SERVFAIL, REFUSED, no answer within the timeout, its port closed) is followed by the next, and CNAME chains are
followed within the answer. The addresses are sorted by RFC 6724, as `getaddrinfo()` sorts them. Its errors are
`dns::errc`'s: `name_not_found` (NXDOMAIN under every name tried) and `no_address` match `condition::not_found`,
`server_failure` (no server answered, or they failed) `condition::temporary_failure`, and `refused` and `bad_response`
(e.g. a CNAME loop) neither. Deadlines, stop tokens, `request_stop()` and sharing a lookup work as for the system's, but
a stopped lookup's queries end at once.

`backend::automatic` (the default) uses the native back end where it gives the answers the system's would, and the
system's elsewhere, as Go's resolver chooses between its own and cgo's:

- On macOS (whose configuration isn't all in `/etc`: per-interface resolvers for VPNs, mDNS), and on OpenBSD (which
  orders the hosts table and DNS with resolv.conf's `lookup`, not nsswitch.conf), always the system's.
- On Linux and the other BSDs, the native, unless the configuration has something it doesn't do: an option (or
  keyword) in resolv.conf it doesn't know; `LOCALDOMAIN` set (even empty), or `RES_OPTIONS` or `HOSTALIASES` not empty,
  in the environment; in nsswitch.conf's `hosts:` line, an action it can't read, `files` or `dns` with an action
  besides the defaults (e.g. `[!UNAVAIL=return]`, or `[NOTFOUND=return]` before another source), or a source besides
  `files`, `dns`, `myhostname` and `mdns*` (e.g. systemd's `resolve` or `mymachines`, as Arch's and Fedora's default
  nsswitch.conf have). `myhostname` and `mdns*` sources only matter to the names they answer, which go to the system's:
  `myhostname` to the machine's own name, `localhost` (and names under it), `_gateway` and `_outbound`; `mdns*` to
  `.local` names (to every name, if `/etc/mdns.allow` is there). So a desktop's `files mdns4_minimal [NOTFOUND=return]
  dns myhostname` (Debian's and Ubuntu's) is the native's for most names, and a server's `files dns` for all.
- `NET_DNS=system` or `NET_DNS=native` in the environment (Go's `GODEBUG=netdns=cgo|go`) makes it that one, wherever;
  other values are ignored. The environment is read when the resolver is made (for `default_resolver()`, when it's
  first used).

The configuration it chooses from is the native back end's: the one given in `options::conf`, or the files, read again
as they change (at most every 5 seconds), so the choice follows them. Concurrent calls for the same name share a
lookup only if they chose the same back end.

## UDP

```cpp
// a server's side: bound to an address and port (0 lets the OS choose: server->local_port()).
auto server = net::udp_socket::bind(&scheduler, net::ipv4_addr::loopback(), 5353);

std::array<std::byte, 1232> buf{};
auto got = co_await server->receive_from(buf);   // std::expected<udp_socket::datagram, std::error_code>
if (got) co_await server->send_to(std::span{buf}.first(got->size), got->addr, got->port);

// a client's side: connected to one peer (nothing is sent, so it doesn't wait), with read() and write().
auto client = net::udp_socket::connect(&scheduler, net::ipv4_addr::loopback(), 5353);
```

A read or `receive_from()` is one datagram (cut short if it's longer than the buffer), and a write or `send_to()` sends
one. A connected socket only reads its peer's datagrams, and an ICMP refusal of one it sent fails its next read with
`std::errc::connection_refused`. An empty datagram is one of size 0 to `receive_from()`, but reads as
`io::errc::end_of_stream` with `read()`, which ends nothing here. Deadlines, timeouts and stop tokens apply as they do to
TCP's sockets.

## HTTP client

`http::client` (`http/client.hpp`) sends HTTP/1.1 and HTTP/2 requests, to `http://` and `https://` URLs, reusing
connections within `client_options`' limits (per host, altogether, idle time), with timeouts for connecting
(`connect_timeout`, which resolving the host counts towards, with `client_options::resolver`, by default
`dns::default_resolver()`), the TLS handshake, the response's head, and stalled reads and writes.

```cpp
net::http::client client{&scheduler};

auto resp = co_await client.get("https://example.com/");   // std::expected<client_response, std::error_code>
if (!resp) co_return;

auto body = co_await net::io::read_all(*resp->body, 1024 * 1024);
```

For other requests, fill in a `client_request` (GET and HTTP/1.1 by default) and `send()` it, which takes it:

```cpp
net::http::client_request req{.method = net::http::request_method::POST};
req.uri = net::url::parse("http://example.com/items").value();
req.headers.set("Content-Type", "application/json");
req.set_body(R"({"name": "thing"})");

auto resp = co_await client.send(std::move(req));
```

A response's connection is reused once its body has been read to the end, and closed if the response is destroyed
first. A body cut short (the connection ending before all of its Content-Length, or its last chunk, came) fails, with
`std::errc::connection_aborted`, rather than ending: `io::errc::end_of_stream` means it was all read.

HTTP/2 is used over TLS with servers that agree on it with ALPN (unless `client_options::http2` is off), and over
cleartext only when `client_options::http2_prior_knowledge` says the server speaks it. A response's `version` says
which was used. An HTTP/2 connection is shared by the host's requests, each on a stream of its own, up to the
server's limit on streams; then another connection is opened (within `max_connections_per_host`), or, with
`strict_max_concurrent_streams`, requests wait for a stream. Its responses' bodies may be read in any order, or not at
all: each stream holds at most its window (64 KiB) of a body not yet read, and the connection's window has room for
every stream's. A response destroyed before its body's end resets just its stream. Requests to a host known to speak
HTTP/2 (one of its connections agreed on it, or with prior knowledge) wait for a connection being made to it, rather
than each making their own. As Go's client does, it knows that of a host only while it has connections: a burst of
requests to a host with none connects in parallel. A request the server refused without processing it is sent again,
once, on another connection, if it has no body (or if nothing of it was sent yet); so is one safe to repeat, without a
body, whose connection dropped (without GOAWAY) before its response. The request's `Host`, if it has one, is its
`:authority`. A request's version and `Connection` field are HTTP/1.x's, and mean nothing over HTTP/2: turn
`client_options::http2` off to send HTTP/1.0, or close a connection after a request. HTTP/2 connections count against
`max_connections` as others do (one without streams is idle, and may be closed to make room for another host's), close
once idle for `idle_timeout` (at once, if it's 0), and fail if their writes stall for `io_timeout`. Destroying the
client lets the bodies being read finish.

A request is canceled by its `stop` token (`client_request::stop`, or `get(url, stop)`): requesting stop ends it
wherever it is (waiting for a connection or an HTTP/2 stream, resolving the host, connecting and the TLS handshake,
sending the request, waiting for the response's head), failing with `std::errc::operation_canceled`, and once `send()`
has returned, the response's body reads fail the same way, unless it was read to its end first. A canceled HTTP/1.1
request's connection is closed, as it may be part way through a request or a response; an HTTP/2 request's stream is
reset (CANCEL), and the connection goes on. A canceled request isn't sent again. Reading the request's body can't be
interrupted: stop requested meanwhile is seen once it returns. A handler passes its own request's token on, so its
outgoing requests end with it:

```cpp
net::http::client_request out{.uri = net::url::parse("http://backend/items").value(), .stop = req.stop};
auto resp = co_await client.send(std::move(out));   // operation_canceled, once req.stop is stopped
```

For `https://`, the server's certificate is verified against the system's trusted certificate authorities, or those
in `client_options::tls` (e.g. `options.tls.trusted_ca_file = "ca.pem"`, for a private CA). A failed verification
compares equal to `tls::alert::bad_certificate`, and says why in its message. Without TLS (`NET_TLS` off), `https://`
fails with `std::errc::protocol_not_supported`.

## HTTP server

```cpp
net::http::router router;
router.GET("/users/:id",
           [](const net::http::server_request& req, net::http::response_writer& resp) -> net::coro::task<void>
           {
               const auto msg = std::format("user {}\n", req.path_value("id"));

               resp.headers().set("Content-Type", "text/plain");
               auto* body = co_await resp.send(net::http::status::OK, msg.size());
               co_await net::io::write_all(*body, msg);
           });

net::http::server server{&scheduler, std::move(router), {.port = "8080"}};
scheduler.spawn(server.serve());
```

- **Routes** (`http/router.hpp`) are paths whose `:name` segments are parameters (`req.path_value("name")`).
  Literal segments win over parameters. `router.subrouter("/api", [](auto& api) { api.GET(...); })` groups routes.
  `http::static_router` (`http/static_router.hpp`) routes the same way, checked at compile time. Either can run
  middleware around some of its routes only (below).
- **A handler** gets the request (as `const`, or not) and a `response_writer`. `resp.send(status, length)` sends the
  head, and returns the writer for a body of that length; `resp.send(status)` is for a body whose length isn't known
  (sent chunked). A handler that sends nothing gets an empty 200; one that throws, a 500. `send()` throws
  `std::system_error` (`std::errc::invalid_argument`) if a header can't be sent (e.g. a value with a line break),
  over HTTP/1.1 and HTTP/2 alike.
- **The request's body** is `req.body`, a reader. Whatever the handler doesn't read is skipped. As a response's, it
  ends (`io::errc::end_of_stream`) only once all of it has been read: one the client cuts short fails (with
  `std::errc::connection_aborted`, if the connection ends first).
- **Canceling:** `req.stop`, a `std::stop_token`, is stopped once the request is no longer wanted: over HTTP/2, when the
  client resets its stream, the stream times out, or its connection ends; over HTTP/1.1, when the client leaves (below),
  or the connection ends; and when the server is aborted (below). Canceling is cooperative: a handler checks
  `req.stop.stop_requested()` between steps, registers a `std::stop_callback` (quick, and not blocking: it runs on
  whichever thread stopped the token), or passes the token on to the waits that take one, which then fail with
  `std::errc::operation_canceled`:

  ```cpp
  if (auto err = co_await scheduler->sleep_until(net::io::clock::now() + 1s, req.stop)) co_return;   // canceled
  ```

  The server doesn't answer for a handler that's stopped without answering (nor send a 500 if it throws): the
  connection's closed, or the stream reset. A handler stopped by the server's `abort()` may still answer (e.g. a 503).
  A copy of the token stays valid after the handler returns. The server sets `req.stop` before calling the handler, so
  middleware can swap in a token of its own (e.g. a `util::linked_stop_source`'s, stopped by a deadline as well as by
  the request's, as `http::timeout` does: below) for the handler it wraps. `req.aborted` is stopped once the server is aborted: not a reason,
  only whether the server's own stop came, for telling it from the client's side.

  An HTTP/1.1 server reads the connection while a handler waits (once it has suspended, and has read the request's
  body to its end), so that the client leaving stops `req.stop`: the end of the stream (as Go's server, which can't
  tell a client that has only stopped sending from one that has gone: its response is still sent), a reset, or TLS's
  close_notify. Over TLS 1.3 the server holds its answer to that back until it has responded (as RFC 8446 6.1 allows),
  so a response still goes out; over TLS 1.2 it's answered at once (as RFC 5246 requires), after which a handler
  stopped that way can't respond. A request that arrives meanwhile isn't a leaving, and is served next. A handler that
  runs straight through, never suspending, isn't watched: it's done before a watch would help.
- **Middleware** runs around a handler. `http::chain(middleware..., handler)` (`http/middleware.hpp`) composes them
  at compile time into one handler, which the server takes (moved in, if it can't be copied, as a router can't); the
  first middleware runs outermost:

  ```cpp
  net::http::server server{&scheduler, std::move(*listener),
                           net::http::chain(net::instrument::http_metrics{}, require_key{"secret"}, std::move(router))};
  ```

  A middleware is a value (its options, and any state its copies share) with a `const` `operator()` that takes the
  request, the response and `next`, the rest of the chain, which `next(req, resp)` runs:

  ```cpp
  struct require_key
  {
      std::string key;

      template<class Next>
      net::coro::task<void>
      operator()(net::http::server_request& req, net::http::response_writer& resp, Next& next) const
      {
          if (req.headers.get("X-Key") != key)
          {
              co_await resp.send(net::http::status::UNAUTHORIZED, 0);
              co_return;
          }

          co_await next(req, resp);
      }
  };
  ```

  It may answer without calling `next` (as above), or `return next(req, resp);` without being a coroutine itself, if
  its work is all before (e.g. putting a value in `req.extensions`, which is how middleware passes values to
  handlers). It calls `next` at most once, and may swap `req.stop` for the rest of the chain (putting it back after).
  `Next` is the chain's own object, called directly, so Clang puts the inner layers' coroutine frames in the outermost
  one's (and a handler's, though not a router's handlers'). A middleware whose body belongs in a `.cpp` takes an
  `http::next_handler` instead, in an `operator()` that isn't a template: a reference to the rest of the chain, called
  indirectly (so the rest's frame is allocated, as a `std::function`'s would be). Or it keeps a few lines of template
  in its header, calling functions in the `.cpp` before and after `next`, as the instrumentation does.

  Middleware can also run around some routes only, after routing (so it sees `req.route` and the path values), and
  not for 404s and 405s. In a `static_router`, `use(...)` comes first in a group (or first in `make_static_router`,
  for every route), and nested groups' middleware runs outermost first:

  ```cpp
  using net::http::literals::operator""_route;

  auto routes = net::http::make_static_router(
      net::http::GET("/health"_route, health),
      net::http::group("/api"_route,
                       net::http::use(require_key{"secret"}),
                       net::http::GET("/users/:id"_route, get_user)));
  ```

  Each route's middleware is composed with its handler at compile time, and Clang puts the layers' and the handler's
  coroutine frames in one: middleware on routes costs no allocation per request beyond the handler's own, where
  `chain()` around a router costs one (a router's handlers aren't elided into the layer that calls them). That holds
  for a handler that's a lambda or a function object: one given as a function is called through a pointer, so its
  frame is allocated (a lambda calling it isn't). Each route holds its own copy of the middleware. In the runtime
  router, `use()` on the router or a subrouter applies to all of its routes, and its subrouters', whenever they were
  added:

  ```cpp
  router.subrouter("/api", [](net::http::router& api) { api.use(require_key{"secret"}).GET("/users/:id", get_user); });
  ```

  Its handlers are type-erased, and so is its middleware: each layer costs an allocation per request.

  `instrument::http_metrics` (`instrument/middleware.hpp`) measures requests with Prometheus metrics, and
  `instrument::http_tracing{on_end}` traces them. Both record a request canceled before its response was sent as 499
  Client Closed Request (not an error) if the client's side stopped it, or as 503 Service Unavailable (an error, as
  any 5xx) if the server was aborted.

  `http::timeout{&scheduler, duration}` (`http/timeout.hpp`) gives the rest of its chain a deadline, as Go's
  `TimeoutHandler` does: it swaps `req.stop` for a `util::linked_stop_source`'s token (so the request's own stops
  still reach the handler), stopped at the deadline by the scheduler's `call_at()`, and puts the request's back after
  (also if the handler throws). Canceling stays cooperative: at the deadline the handler is only asked to stop (its
  waits given `req.stop` end), and once it returns, if the deadline passed and it sent no response (and the request
  wasn't stopped otherwise), the timeout answers 503 Service Unavailable with an empty body. A response the handler
  sent stands. Unlike Go's, it doesn't buffer the handler's output, so it never writes while the handler might, and a
  handler that ignores its token delays the 503 until it returns (holding its connection, or its HTTP/2 stream's
  place, meanwhile). It's also what ends an HTTP/2 handler that waits with its stream open, sending nothing, which no
  stream timeout does. Put the instrumentation outside it, so it records the 503 sent rather than the timeout's token
  stopped (a 499):

  ```cpp
  net::http::chain(net::instrument::http_metrics{}, net::http::timeout{&scheduler, 5s}, std::move(router))
  ```

  It costs a request an allocation (the linked source's state) and a timer (a node in the scheduler's timers, reused,
  added and removed under a lock).
- **`server_config`** sets the host and port, and limits: header and body read timeouts, the write timeout, the size
  of a request's head, and how many connections are served at once. To handle a failure to listen, give the server a
  listener from `listener::make()` instead of a host and port.
- **Stopping:** `server.close()` stops accepting and ends the connections gracefully: a connection waiting for a
  request (its first, or the next on a kept-alive one) closes at once, and one in the middle of a request finishes it
  (the handler reads the body and sends its response as usual, which says `Connection: close` if it wasn't sent yet),
  then closes, without serving requests pipelined after it; HTTP/2 connections are below. A `serve()` task that
  hasn't started yet returns at once when it does. `close()` cancels nothing. `co_await server.join()` waits for
  `serve()` and the connections to finish; `server.abort()` does what `close()` does and stops every handler's
  `req.stop` too, and ends HTTP/1.1 requests in progress (reading a body fails), for a program that gives the
  requests a while and then gives up (as Go's `Shutdown` then `Close`). Destroying the server aborts it, and waits for
  its connections; so does the scheduler's `request_stop()` (or `shutdown()`), which also cancels what the
  connections are waiting for. `example/http-server` drains for up to 10 seconds:

  ```cpp
  server.close();
  scheduler.run_to_completion(drain(&scheduler, &server, std::chrono::seconds{10}));   // join(), or abort() after 10 s
  ```
- **HTTP/2** is served too (unless `server_config::http2` is off), with the same handlers: over TLS to clients that
  agree on `h2` with ALPN (browsers do), and over cleartext to clients that start with HTTP/2's preface (prior
  knowledge, e.g. `curl --http2-prior-knowledge`, or a proxy in front). A request's `version` says which (2.0). Its
  streams are served at once, each handler in a task of its own, up to `http2_max_concurrent_streams` at a time
  (more wait for others to finish); the timeouts apply to each stream (one that times out is reset, and the
  connection's others go on), and `close()` ends HTTP/2 connections gracefully (GOAWAY, then once their streams are
  done; the client's GOAWAY doesn't stop the streams it has open either). A connection that ends otherwise (the
  client closing it, or an error) is closed at once, and its handlers' tokens stopped; the handlers still hold its
  place among `max_connections` until they return. A handler that throws once its response is under way has its
  stream reset, so the client doesn't take what it got as the whole response; one the server's `abort()` stopped, with
  its response incomplete, has it reset with CANCEL.
- **HTTPS:** give `server_config::tls` a `tls::server_context`, made from the certificate chain and private key (PEM
  files). Making it reports a file that can't be loaded as an error, before the server starts:

  ```cpp
  auto ctx = net::tls::server_context::make({.certificate_chain = "chain.pem", .private_key = "key.pem"});
  if (!ctx.has_value()) { /* ctx.error() */ }

  net::http::server server{&scheduler, std::move(*listener), std::move(router), {.tls = std::move(*ctx)}};
  ```

  Each connection's handshake has `header_read_timeout` to finish (and each of its waits for the client is limited by
  `body_read_timeout` and `write_timeout`), and `req.tls_version` tells a handler the TLS version its request's
  connection agreed on (`net::tls::version::tls12` or `tls13`; `none` in cleartext, and in builds without TLS, so
  `req.tls_version != net::tls::version::none` is whether it came over TLS). The server offers
  `h2` and `http/1.1` with ALPN (or only `http/1.1` if `http2` is off). `example/http-server` serves HTTPS with
  `--cert` and `--key`.

## TLS

The HTTP client uses TLS for `https://` URLs by itself. To use it directly, TLS (`tls/tls.hpp`, built unless `NET_TLS`
is off, when `NET_HAS_TLS` is defined) runs over a TCP socket:

```cpp
auto ctx = net::tls::client_context::make();        // the system's trusted CAs (or client_options' CA file)
auto sock = co_await net::tcp_socket::connect(&scheduler, "example.com", "443");
auto tls = co_await net::tls::stream::connect(std::move(*sock), *ctx, "example.com",
                                              net::io::clock::now() + std::chrono::seconds{10});
// tls->read(), tls->write(), ..., and co_await tls->close() to end it with close_notify.
```

- The server's certificate is verified against the host name. `tls::client_options::insecure_skip_verification` (the
  HTTP client's `client_options::tls.insecure_skip_verification`) turns that off, for testing.
- Errors compare with `tls::alert` conditions (e.g. `err == tls::alert::bad_certificate`), whichever side sent the
  alert; `tls::from_peer(err)` says which. A certificate that failed verification says why in its message (e.g.
  "Certificate does not match provided name").
- A server makes a `server_context` from its certificate chain and key, and calls `stream::accept()` (as
  `http::server` does).
- Both contexts allow TLS 1.2 and 1.3 by default. `min_version` and `max_version` (in `client_options` and
  `server_options`, as Go's `MinVersion` and `MaxVersion`; e.g. `.min_version = net::tls::version::tls13`) narrow that,
  and `make()` fails with `std::errc::invalid_argument` if the minimum is after the maximum. The client offers the
  latest it allows, and the server agrees on the latest of those it allows; with none in common, the server fails the
  handshake with `protocol_version` (so the client's error is that alert from the peer). `stream::version()` is the
  version agreed on, and a server's handlers see it in `server_request::tls_version`. The HTTP client's
  `client_options::tls` and the server's `server_context` (in `server_config::tls`) carry them. `tls::version` is in
  `tls/version.hpp`, which builds without TLS too; its `none` (no TLS) isn't a version a context may allow.
- `stream::connect()` takes an optional `std::stop_token`, as `stream::accept()` does in its `accept_options` (`stop`):
  requesting stop ends a handshake that's waiting, with `std::errc::operation_canceled`. `stream::set_stop_token()` does
  the same for reads and writes (as a socket's): a canceled read can be tried again, but a canceled write fails the
  writes after it, as any failed write.
- The peer's close_notify is answered at once, after which nothing more can be written. `accept_options`'
  `hold_close_reply` holds the answer to the client's back, so the stream can still be written after it, until
  `shutdown()` or `close()` (TLS 1.3 only: TLS 1.2 requires the answer at once); `release_close_reply()` goes back to
  answering at once. `accept_options::alpn` offers other ALPN protocols than the context's.
- A read and writes may run at once (e.g. HTTP/2's reader and writer), but only one read. A write returns once what it
  wrote has gone to the socket (after what was queued before it, e.g. another write, or a read's reply to the peer), so
  the socket's write deadline and timeout limit it; a write that fails fails every write after it (part of a record
  may have gone).
- `co_await tls->shutdown()` sends close_notify without closing the socket, so what the peer sent before its own
  close_notify can still be read. The peer's close_notify ends the stream (`io::errc::end_of_stream`), and is answered
  with close_notify, so nothing more can be written either.

## Instrumentation

- **Prometheus** (`instrument/prometheus/`): counters, gauges and histograms, registered with
  `prometheus::registry::register_metric()`. `registry::record(writer)` writes them all, in Prometheus' text format,
  e.g. in a `/metrics` handler, with `resp.send(status::OK)`, as the length isn't known.
  `register_process_metrics()` adds the process's (CPU, memory, file descriptors, ...).
- **OpenTelemetry** (`instrument/opentelemetry/`): trace context propagation, and server spans with the
  `http_tracing` middleware.

## Which headers to include

| Header                                 | For                                                       |
|----------------------------------------|-----------------------------------------------------------|
| `coro/thread_pool.hpp`, `io/scheduler.hpp` | the runtime                                           |
| `coro/task.hpp`                        | `coro::task`                                              |
| `io/reader.hpp`, `io/writer.hpp`, `io/util.hpp` | reading and writing (`write_all`, `read_all`, `readline`) |
| `io/io.hpp`                            | `io::result`, `io::errc`, `io::status_condition`          |
| `listen.hpp`, `tcp.hpp`                | TCP sockets                                                 |
| `udp.hpp`                              | UDP sockets                                               |
| `dns/resolver.hpp` (and `dns/config.hpp`), `ip_addr.hpp`, `url.hpp` | names, addresses and URLs  |
| `http/client.hpp`                      | the HTTP client                                           |
| `http/server.hpp`, `http/router.hpp`, `http/static_router.hpp`, `http/middleware.hpp`, `http/timeout.hpp` | the HTTP server |
| `util/linked_stop_source.hpp`          | stop sources that a parent token also stops               |
| `tls/tls.hpp`                          | TLS                                                       |
| `tls/version.hpp`                      | `tls::version` (included by `tls/tls.hpp` and `http/request.hpp`; without TLS too) |
| `instrument/...`                       | metrics and tracing                                       |
| `exception.hpp`                        | `gai_category()`                                          |

The rest are what those are built from (e.g. `http/http11.hpp`, the HTTP/1.1 codec; and HTTP/2's, `http/hpack.hpp`
and `http/http2*.hpp`, of which `http/server.hpp` and `http/client.hpp` include only `http/http2_config.hpp`, for their
configurations). `detail` namespaces, and what the headers' comments say is for the scheduler or I/O objects (e.g.
`thread_pool::poller`), aren't for applications.
