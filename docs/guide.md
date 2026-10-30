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
library's own (`io::errc`, `tls::errc`, `dns::errc`, `gai_category()`'s, `http::client_errc`, ...).

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
- The HTTP client's own errors are `http::client_errc`'s (`http/client.hpp`, in `http::client_category()`):
  `too_many_redirects` and `bad_redirect` (see HTTP client).
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

// or connects to addresses, without resolving anything.
const std::array<net::ip_addr, 2> addresses{net::ipv6_addr::loopback(), net::ipv4_addr::loopback()};
auto local = co_await net::tcp_socket::connect(&scheduler, addresses, 7777);
```

`connect` races a host's addresses, as RFC 8305 (Happy Eyeballs) has it: in their order (the resolver's, sorted by RFC
6724), but interleaved by family, the first address's first (e.g. IPv6, IPv4, IPv6, ...), it starts an attempt every
`connect_options::attempt_delay` (250 ms), or at once when the latest attempt fails, and the attempts go on together,
each until the timeout's end; the first to connect wins, and the rest are stopped and closed. So an address that doesn't
answer (a broken IPv6 route, say) costs the delay, rather than the timeout. Usually the first attempt connects within
the delay, and then nothing else happens (no task, no allocation). An `attempt_delay` of 0 tries the addresses one at a
time, in their order, each with a share of the timeout, as Go's dialer gives it: what's left over the addresses left,
but at least 2 seconds (never past the end), so an address that doesn't answer leaves time for the rest. When all fail,
the error is the last attempt's to fail. A name is resolved with `connect_options::resolver` (by default
`dns::default_resolver()`, see DNS), within the same timeout, and stop token. Resolving both families, `connect` starts
on the first family's addresses rather than waiting for both (RFC 8305 section 3): IPv6's at once, IPv4's once they've
waited 50 ms (the Resolution Delay) for IPv6's, or as soon as those come in; addresses that come in later are sorted
into the attempts not yet made (going on alternating by family), and if every attempt has failed before then, it waits
for them. With `attempt_delay` 0, or `proto` limiting it to one family, the name is resolved before connecting, as
`resolve()` does. An IP address (e.g. `"127.0.0.1"` or
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

`co_await socket.wait_writable()` waits until the kernel counts the socket writable, writing nothing (its result's
count is 0): at once, without a coroutine, if `poll()` says so; otherwise as a write waits, so the write deadline,
timeout and stop token apply, and it can't wait alongside a write (whichever comes second fails with
`std::errc::device_or_resource_busy`). With `tcp_socket::set_notsent_lowat(bytes)` (TCP_NOTSENT_LOWAT; 0 turns it off),
a TCP socket counts as writable only while what's written but not yet sent is under the mark (on Linux, once it's under
half of it), so a writer that waits for that before each write keeps little queued in the kernel (where it can no
longer be reordered, e.g. by priority), however large the send buffer grows, without limiting what's in flight. Linux's
`send()` stops at the mark by itself, but macOS's doesn't, so wait before writing:

```cpp
(void)sock->set_nodelay();
(void)sock->set_notsent_lowat(16 * 1024);   // std::errc::not_supported where there's no such option

while (/* more to write */)
{
    if (auto ready = co_await sock->wait_writable(); ready.err) { /* timed out, canceled, ... */ }
    co_await net::io::write_all(*sock, next_chunk());   // chosen as late as possible
}
```

## DNS

```cpp
// a name's addresses: std::expected<std::vector<ip_addr>, std::error_code>.
auto addrs = co_await net::dns::default_resolver().resolve(
    &scheduler, "example.com", {.deadline = net::io::clock::now() + std::chrono::seconds{5}, .stop = stop});
if (!addrs && addrs.error() == net::dns::condition::not_found) { /* no such name, or no addresses */ }
```

A `dns::resolver` (`dns/resolver.hpp`) looks names up with the system's resolver (`getaddrinfo()`, on the scheduler's
threads for blocking calls) or its own (the native back end, below), choosing per name as Go does (`backend::automatic`,
below); `dns::default_resolver()` is the one `connect` and the HTTP client use unless given another. The system's looks
up both families with a call for each (`AF_INET6` and `AF_INET`, in parallel, on two threads), so `connect` can start on
the first to answer, and sorts their addresses together by RFC 6724, as one call would have. One family failing (no
AAAA records, say, or no IPv6 on the host) isn't an error if the other has addresses; if both fail, the error is a
temporary failure's if only one is one (e.g. `EAI_AGAIN`: the name may have addresses), and otherwise IPv4's.
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
replies that don't match a query (ID, question) are ignored (and `connect` is told of a family's addresses as soon as
they're in). A truncated answer is asked again over TCP, a server that
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

## URLs

`net::url` (`url.hpp`) holds a URL's parts: `url::parse()` returns `std::expected<url, url_parse_failure>` (which part
failed, and where). The scheme and port, and the user info and host (an IPv6 one without its brackets), are plain
text; the path, query (without its `?`) and fragment (without its `#`) are kept as they were written, percent-encodings
and all: `%2F` stays `%2F` (rather than becoming a `/`, another path), `%2f` stays `%2f`, and a query's parameters stay
in their order, with their `+`s. So the HTTP client sends a URL as it was given (its request line is
`origin_form()`, the path and query as they are), as a signed URL (e.g. a CDN's, or an object store's), whose
signature covers them as written, needs. Only what can't appear unencoded (a space, a control or non-ASCII byte, a `%`
that doesn't start an encoding, or another character RFC 3986 doesn't allow there) is encoded, by `parse()` and again
by `build()` and `origin_form()`, so nothing set in a part can end a request line. `url::parse_query(u.query)` decodes
the query's values (as a form: `+` is a space). Setting a part, encode what's meant as data rather than a delimiter,
with `url::encode()`:

```cpp
auto u = net::url::parse("https://example.com/files/a%2Fb?sig=ab+c").value(); // u.path == "/files/a%2Fb"
auto values = net::url::parse_query(u.query);                                 // values["sig"] == {"ab c"}
u.query = "q=" + net::url::encode("a&b c");                                   // "q=a%26b%20c"
auto next = net::url::resolve(u, "../other?x=1");                             // https://example.com/other?x=1
```

`parse()` reads a URL with neither a scheme nor `//`, not starting with `/`, as starting with its host
(`example.com/path`), for `client::get("example.com")`. `url::resolve(base, reference)` reads a reference as RFC 3986
does (`next/page` is a relative path), and resolves it against `base` (section 5.2: e.g. a `Location`), removing `.`
and `..` segments (not their encodings), and keeping the rest as written; it returns `std::expected<url,
std::error_code>`, failing with `std::errc::invalid_argument` for a reference that isn't valid, or a base with neither
a scheme nor a host. An empty query or fragment is none (`/path?` builds as `/path`).

## HTTP client

`http::client` (`http/client.hpp`) sends HTTP/1.1 and HTTP/2 requests, to `http://` and `https://` URLs, reusing
connections within `client_options`' limits (per host, altogether, idle time), with timeouts for connecting
(`connect_timeout`, which resolving the host counts towards, with `client_options::resolver`, by default
`dns::default_resolver()`; a host's addresses are raced as `connect` races them, `client_options::attempt_delay` apart),
the TLS handshake, the response's head, and stalled reads and writes.

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
every stream's. Request bodies that contend for a connection take turns by their requests' `Priority` fields (see
Priority, under the server, below), read as the server reads them (what a field leaves out, or all of it if it doesn't
parse, is RFC 9218's default, `u=3` not incremental): the most urgent first, and of one urgency, incremental ones a
frame each, others one at a time, in the order they were sent (the two kinds taking turns). A request without the field
counts as `u=3, i`, so bodies sent without them share the connection, a frame each. A body alone goes as it's
written; a body waiting its turn while others' are sent isn't counted against `io_timeout`, which bounds only a stall.
As the server's, the client's HTTP/2 connections set TCP_NOTSENT_LOWAT on their sockets
(`client_options::http2_notsent_lowat`, 16 KiB; over TLS, the TCP socket's; not on HTTP/1.1's; 0 sets none), and their
writers wait for the kernel to hold less than that before framing more, so a body that's more urgent isn't queued
behind what the send buffer holds of others'.
Once `send()` has returned, `resp->set_priority({.urgency = 7})` asks the server to send the rest of the response with
another priority (RFC 9218's `PRIORITY_UPDATE`; e.g. a download that's gone to the background, or a request the user's
now waiting for): a request's field is its first priority, and the update replaces it (what it leaves out is the
default, not what the field said). It sends something only when the priority changes, and updates made faster than the
connection's writer takes them go as their last. It returns false, sending nothing, over HTTP/1.1, for a body the
caller put in place of the client's, and once the response has ended (all of it's come) or failed; the server may
ignore it (cppnet's takes it, see Priority below). Any thread may call it, while the body's being read too.
A response destroyed before its body's end resets just its stream. Requests to a host known to speak
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

With `client_options::http2_coalescing` (off by default), a request to an `https://` origin whose host has no HTTP/2
connection with a stream free (nor an idle HTTP/1.1 one) may take a stream of another host's, rather than connecting
(connection coalescing): one whose server has listed the request's origin in an ORIGIN frame (RFC 8336; e.g. a CDN
serving many names from one certificate, or cppnet's server with `server_config::http2_origins`, below), and whose
certificate is valid for the request's host, checked as a new connection's handshake would check it (once per host and
connection, on the requesting task's thread). The host isn't looked up. Only ORIGIN coalesces (browsers also coalesce
hosts whose addresses are the connection's: not cppnet), never over cleartext, nor with `tls.insecure_skip_verification`
(nothing then says whose a server is). The connection still counts as its own host's (`stats()`,
`max_connections_per_host`, `idle_timeout`). It's off by default as the host isn't resolved: whoever holds a certificate
valid for a name (mis-issued, or with a stolen key) could, by listing it, take a client's requests for it without being
on the way to its server (RFC 8336, section 4: the client checks neither Certificate Transparency nor revocation), and a
client that fetches URLs others choose (webhooks, a crawler) in the process that sends its own credentialed requests
would let such a server have both. A server that gets a request for an origin it doesn't serve answers 421 (Misdirected
Request): the origin's then dropped from the connection's set, the 421 discarded, and the request sent again, once, on a
connection of its own host's (with its body, read again from `client_request::get_body`, as for a 307; without it, a
request with a body gets the 421 back). A 421 on a connection of the request's own host's is returned as it is.

```cpp
net::http::client client{&scheduler, {.http2_coalescing = true}};

auto a = co_await client.get("https://www.example.com/");      // connects: its server lists static.example.com
auto b = co_await client.get("https://static.example.com/x");  // on www's connection, if its certificate has the name
```

Redirects (301, 302, 303, 307 and 308, with a `Location`) are followed only when asked for: up to
`client_options::max_redirects` of them (0 by default, so `send()` returns the redirect's response; Go's 10 is a
sensible limit), or `client_request::max_redirects`, which overrides it for one request. Each is a request of its own
(its connection, retries and timeouts; the request's stop token covers them all), to `Location` resolved against the
request's URL (`url::resolve()`, keeping the fragment if `Location` has none), and the response's `url` says which URL
it answers. One more than the limit fails with `http::client_errc::too_many_redirects`, a `Location` that isn't a valid
reference with `client_errc::bad_redirect`, and one that isn't `http` or `https` with `std::errc::protocol_not_supported`.

```cpp
net::http::client client{&scheduler, {.max_redirects = 10}};

auto resp = co_await client.get("http://example.com/download");   // after any redirects
if (resp) std::println("{}", resp->url.build());                    // e.g. a CDN's signed URL
```

301 and 302 (for methods other than GET and HEAD) and 303 (other than HEAD) are followed with a GET, and none of them
sends the body, nor the fields describing it (`Content-Length`, `Content-Type`, ...); 307 and 308 keep the method and
the body, which they read again from `client_request::get_body` (a function returning a new reader of it, as Go's
`GetBody`), which `set_body()` sets: a request with a body but no `get_body` gets the 307 or 308 back. The request's
fields go on, but `Authorization`, `Proxy-Authorization` and `Cookie` only to the same scheme, host and port, or the
same host from `http` to `https` on the default ports, and once dropped, to none of the later requests; a `Host` the
request set only to the same host and port. No `Referer` is sent. A redirect's response is read to its end and
discarded if what's left of its body is at most 2 KiB (so its HTTP/1.1 connection is reused), and closed otherwise (an
HTTP/2 stream is reset).

Interim (1xx) responses before the final one (e.g. 100 Continue, or 103 Early Hints) are skipped, unless the request has
`client_request::on_informational`: then it's called with each one's status and fields (valid only during the call), in
order, on the task sending the request, before `send()` returns, over HTTP/1.1 and HTTP/2 alike, and for the requests
redirects make too. Never for 101, which is final over HTTP/1.1. An exception it throws comes out of `send()`, the
request abandoned (its HTTP/1.1 connection closed, or its HTTP/2 stream reset). E.g. to act on Early Hints while the
server's still working on the response:

```cpp
net::http::client_request req{.uri = net::url::parse("https://example.com/").value()};
req.on_informational = [&](net::http::status code, const net::http::headers& fields)
{
    if (code != net::http::status::EARLY_HINTS) return;
    if (const auto links = fields.get_all("Link"))
    {
        for (const auto& link : *links) preload(link);   // e.g. start fetching what it hints at
    }
};

auto resp = co_await client.send(std::move(req));   // the final response
```

A response's interim heads are limited altogether, whether or not they're passed on: over HTTP/1.1, with the final head,
to the client's limit on a response's head (1 MiB); over HTTP/2, to 1 MiB (with 32 bytes a field, as HTTP/2 counts them),
past which the stream is reset. Either fails the request with `std::errc::value_too_large`. They don't put off
`response_header_timeout`, which is for the final head. A request that got one isn't sent again if its connection then
ends, as the server had it.

A request with `Expect: 100-continue` (which the client never adds by itself) and a body (not of length 0) waits for the
server's go-ahead before sending the body, over HTTP/1.1 and HTTP/2: its head goes alone (over HTTP/2, without ending
the stream), then the client waits for the server's first response, up to `client_options::expect_continue_timeout`
(1 s; 0 sends the body at once, as for a request without the field). A 100 (Continue), or nothing in time, sends the
body; another interim response (e.g. 103) is passed on, and the wait goes on, to the same deadline. A final response
first (e.g. 413, from a server that won't read the body) is returned, and what becomes of the body is Go's client's
rule. Over HTTP/1.1, it's sent only if the connection's to be reused, as its framing needs it; otherwise it isn't, and
the connection's closed once the response is done with. Over HTTP/2, it isn't sent after a status above 299 (the
stream's reset once the response is done with), and is after a 2xx, once the wait's over, as the server may be reading
and writing at once (`send()` returns once it's gone). A 417 (Expectation Failed) to a request whose body wasn't sent
has the request sent again at once, once, without its `Expect` field (over HTTP/1.1 on another connection, over HTTP/2
on the same one): the body's still unread, so no `get_body` is needed. A 417 after the body was sent (once the wait was
over) is returned. `response_header_timeout` starts once the body's sent; while the client waits over HTTP/1.1, each
head counts against the limit on a response's head by itself (the wait being bounded).

```cpp
net::http::client_request req{.method = net::http::request_method::PUT};
req.uri = net::url::parse("https://example.com/uploads/big.iso").value();
req.headers.set("Expect", "100-continue");
req.set_body(std::move(content));   // sent once the server says to (or doesn't say in time)

auto resp = co_await client.send(std::move(req));   // e.g. 413, without the body having been sent
```

Trailers are fields that come after the body (RFC 9110, section 6.5): e.g. gRPC's `grpc-status`, or a digest of the
body. A response's are `resp->trailers()`, there once its body has been read to its end (a read has reported
`io::errc::end_of_stream`), and empty until then: a chunked body's trailer section over HTTP/1.1, the HEADERS frame
after the body over HTTP/2. They're kept apart from `resp->headers`, without the fields that can't be trailers (see
`net::http::trailer_allowed()`), and are the body's: valid as long as it is (a body put in its place has none). A
request's are filled by `client_request::make_trailers`, called once its body has been read to its end, on the task
sending it, and again each time the body's sent again (a 307 or 308, or a retry): over HTTP/2 for any request (one
without a body sends its head, then them), over HTTP/1.1 only for a body sent chunked (without a Content-Length), and
not called otherwise.

```cpp
auto  upload = std::make_unique<digesting_reader>(file);   // e.g. a reader that hashes what's read of it
auto* digest = upload.get();

net::http::client_request req{.method = net::http::request_method::PUT,
                              .uri    = net::url::parse("https://example.com/upload").value()};
req.body          = std::move(upload);                     // no Content-Length: chunked, over HTTP/1.1
req.make_trailers = [digest](net::http::trailer_fields& trailers) { trailers.set("Content-Digest", digest->value()); };
req.headers.set("TE", "trailers");                         // this caller reads the response's trailers

auto resp = co_await client.send(std::move(req));
if (!resp) co_return;

auto body = co_await net::io::read_all(*resp->body);
if (body && resp->trailers().get("Grpc-Status") != "0") co_return;   // once the body's been read to its end
```

A trailer that can't be sent (e.g. a value with a line break) fails the request with `std::errc::invalid_argument`, cut
short: its HTTP/1.1 connection closed without the last chunk, or its HTTP/2 stream reset, so the server doesn't take it
for complete. The client never sends `TE: trailers` itself (it says the caller won't drop the response's trailers,
which only the caller knows): a caller that relies on them sets it, and over HTTP/1.1 `TE` is then listed in
`Connection`, as RFC 9110 has a sender do; over HTTP/2 only `TE: trailers` is sent. A 301, 302 or 303's request, which
drops the body, drops `make_trailers` with it.

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
compares equal to `tls::alert::bad_certificate`, and says why in its message. For a server that asks for a client
certificate (mutual TLS), give `client_options::tls` one, with its key (`options.tls.certificate_chain = "client.pem";
options.tls.private_key = "client.key";`): one identity per client. A server that refuses it (or wanted one) fails the
request with its alert, e.g. `tls::alert::certificate_required` or `bad_certificate`. Without TLS (`NET_TLS` off),
`https://` fails with `std::errc::protocol_not_supported`.

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
- **Interim responses:** `resp.send(status)` with a 1xx status (e.g. `status::EARLY_HINTS`), before the final
  response, sends an interim one at once, with the headers set so far, which the final response then sends too:

  ```cpp
  resp.headers().add("Link", "</style.css>; rel=preload");
  co_await resp.send(net::http::status::EARLY_HINTS);   // the client can start fetching the stylesheet
  const auto page = co_await render(req);              // meanwhile
  auto*      body = co_await resp.send(net::http::status::OK, page.size());
  co_await net::io::write_all(*body, page);
  ```

  An HTTP/1.0 client gets none (it doesn't know them: `send()` then returns a writer that drops what's written).
  `status::SWITCHING_PROTOCOLS` can't be sent (`std::system_error`, `std::errc::not_supported`): a 101 goes only with a
  connection that really switches, which accepting a WebSocket does (see WebSockets). A request with `Expect:
  100-continue`, whose client may wait before it sends the body, gets a 100 (Continue) from the server when the handler
  first reads the body, unless the handler has sent its own 100 or the final response by then (as Go's server: another
  interim response, e.g. a 103, doesn't stop it); a handler that answers without reading it (e.g. a 413, for a body too
  large) sends none, and over HTTP/1.1 the connection closes after its response, rather than waiting for a body that may
  never come. Any other expectation is answered 417 (Expectation Failed), without calling the handler.
- **The request's body** is `req.body`, a reader. Whatever the handler doesn't read is skipped. As a response's, it
  ends (`io::errc::end_of_stream`) only once all of it has been read: one the client cuts short fails (with
  `std::errc::connection_aborted`, if the connection ends first).
- **Trailers** are fields that come after the body (RFC 9110, section 6.5), e.g. what's known only once the body's done:
  a digest of it, `Server-Timing`, or gRPC's `grpc-status`. A request's are `req.trailers` (a
  `net::http::trailer_fields`: a `headers` holding 2 fields in itself rather than 8, as there are few), kept apart from
  `req.headers`, there once the handler has read the body to its end, and empty until then: a chunked body's trailer
  section over HTTP/1.1, the HEADERS frame after the body over HTTP/2. A response's are `resp.trailers()`, sent when the
  response ends, once the handler has returned (so middleware may add to them after `next()`, declaring them first:
  see below):

  ```cpp
  auto* body = co_await resp.send(net::http::status::OK);   // no Content-Length: chunked, over HTTP/1.1
  co_await net::io::write_all(*body, reply);
  resp.trailers().set("Grpc-Status", "0");                  // sent after the body
  ```

  Over HTTP/1.1 only a chunked body has trailers (in its last chunk), so they're sent only for a body sent with
  `send(status)` to an HTTP/1.1 client, and dropped otherwise: after a body with a Content-Length, to an HTTP/1.0
  client, and for HEAD. A handler with trailers to send uses `send(status)`. Over HTTP/2 they're sent whatever the
  body, but a response with a Content-Length (or without a body) ends with its last DATA frame (or its head), so they
  must be set, or declared by a `Trailer` field, by the time its head is sent: then they follow the body; set later,
  undeclared, they're dropped. So middleware that adds trailers after `next()` declares them before calling it:

  ```cpp
  resp.headers().add("Trailer", "Server-Timing");            // before the head's sent
  co_await next(req, resp);
  resp.trailers().set("Server-Timing", timing);              // sent after the body, with or without a Content-Length
  ```

  A response sent with `send(status)` ends once the handler has returned, so its trailers may be set until then.
  Either way, they're sent whatever the request's `TE`. `net::http::trailer_allowed(name)` is
  whether a field may be a trailer: not one for framing, routing, authentication, request modifiers, response
  controls or the content's format (e.g. `Content-Length`, `Host`, `Authorization`, `Cache-Control`, `Content-Type`,
  `If-*`, `Trailer`), which are dropped, received or sent. A trailer whose name or value can't be sent (e.g. a value
  with a line break) cuts the response short, logged: the HTTP/1.1 connection closes without the last chunk, or the
  HTTP/2 stream's reset, so the client doesn't take the response for complete. The 500 for a handler that throws, and
  `http::timeout`'s 503, have none.
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
  listener from `listener::make()` instead of a host and port. A head over `max_header_bytes` is answered 431 (Request
  Header Fields Too Large) without a handler: over HTTP/1.1 the connection then closes; over HTTP/2 (whose limit counts
  32 bytes more a field, as HTTP/2 counts a header list) only the stream ends, and the connection goes on.
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
- **Priority** (RFC 9218): a request's `Priority` field says how urgent its response is (`u`, 0 to 7, 3 by default) and
  whether the client uses it as it arrives (`i`). `http/priority.hpp` reads and writes it: `parse_priority(value)`
  gives a `priority` (what's missing or invalid keeping the default, or a `base` given as its second argument; what it
  doesn't know ignored; and a value that doesn't parse taken as none), and `to_string()` makes a field's value, e.g.
  for a bulk download's response, which the client also gets:

  ```cpp
  resp.headers().set("Priority", net::http::to_string({.urgency = 7}));   // "u=7"
  ```

  The HTTP/2 server sends responses by their priorities (unless `server_config::http2_priorities` is off): the request's
  field, or the client's `PRIORITY_UPDATE` frames (which replace it, even one sent before the request, and move a
  response under way), then the response's field, merged over it when its head is sent (what it leaves out keeps the
  client's). When a connection's responses contend for it, the most urgent go first (the less urgent wait, for as long
  as there's more urgent data to send); of one urgency, those that aren't incremental go one at a time, in the order
  they were requested, and incremental ones take turns, a frame each (and the two kinds take turns with each other). A
  client that has never sent a priority gets its responses shared alike (as `u=3, i`), rather than one at a time; so
  does one sending `Via`, `Forwarded` or `X-Forwarded-For` (an intermediary, whose `Priority` fields are its clients',
  so ignored). Only what hasn't been handed to the connection's writer can be reordered: a response alone goes at once,
  and a handler's write returns once its data's been framed, in its turn (so it waits while more urgent ones are sent,
  which doesn't count against its `write_timeout` while the server's writing to the client). Nor can what the kernel
  holds, written but not yet sent: so the server sets TCP_NOTSENT_LOWAT on each HTTP/2 connection's socket
  (`server_config::http2_notsent_lowat`, 16 KiB; over TLS, the TCP socket's; not on HTTP/1.1's), and the connection's
  writer waits for the kernel to hold less than that before it frames more (see `socket::wait_writable()`, above). So
  what's past reordering is the mark and a round of frames (64 KiB), rather than the send buffer, which the kernel
  grows up to megabytes; what's in flight isn't limited, so a path with a long round trip still fills its window. 0
  sets none, and the writer writes as the socket takes it. A client sets a request's priority with the same field
  (`req.headers.set("Priority", to_string({.urgency = 1}))`), orders its own request bodies by it (`u=3, i` without
  one: see the client, above), and changes a response's in flight with `client_response::set_priority()`, which sends
  `PRIORITY_UPDATE`. A field sent as several lines is one value, its lines joined with commas: `req.headers.get()`
  gives only the first.
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
  `h2` and `http/1.1` with ALPN (or only `http/1.1` if `http2` is off). Clients' TLS 1.3 sessions are resumed with
  tickets, as the context's options say (see TLS, below), and `http::client` resumes them. `example/http-server`
  serves HTTPS with `--cert` and `--key`.

  For mutual TLS, the context asks clients for their certificates (`client_auth`: `optional` or `required`), verifying
  them against `client_ca_file`'s certificate authorities; a handler sees the chain a client sent in
  `req.peer_certificates` (leaf first; null if it sent none, in cleartext, and in builds without TLS), and authorizes
  from it:

  ```cpp
  auto ctx = net::tls::server_context::make({.certificate_chain = "chain.pem",
                                             .private_key       = "key.pem",
                                             .client_auth       = net::tls::client_auth::required,
                                             .client_ca_file    = "clients-ca.pem"});

  // in a handler:
  const auto& certs = req.peer_certificates;
  if (!certs || !std::ranges::contains(certs->front().uris, "spiffe://example.com/billing")) { /* 403 */ }
  ```
- **ORIGIN** (RFC 8336): `server_config::http2_origins` lists the origins an HTTP/2 connection over TLS says it serves,
  in an ORIGIN frame sent after the server's SETTINGS. A client that coalesces by it (Firefox does, and cppnet's with
  `client_options::http2_coalescing`) may then send requests for any of them on that connection, if the certificate is
  valid for their hosts, without looking the names up or connecting to them. Each is `https://host` or
  `https://host:port` (a DNS name, without wildcards, which ORIGIN can't say; an IPv4 address; or an IPv6 one in
  brackets), normalized (lowercase, the default port 443 left out, repeats dropped); the server's constructors throw
  `std::invalid_argument` for one that isn't, or whose host the context's certificate doesn't cover (clients would
  ignore it). None is sent by default, nor without TLS (RFC 8336 has clients ignore ORIGIN over cleartext), nor over
  HTTP/1.1. Only the application knows which of the certificate's names it serves, so it lists them; and a request for
  an origin it doesn't serve it answers with 421, after which the client sends it on a connection of its own:

  ```cpp
  net::http::server_config cfg{.tls = std::move(*ctx)};
  cfg.http2_origins = {"https://example.com", "https://static.example.com"};

  // in a handler, or middleware in front of the router:
  if (req.host != "example.com" && req.host != "static.example.com")
  {
      co_await resp.send(net::http::status::MISDIRECTED_REQUEST, 0);
      co_return;
  }
  ```

## WebSockets

A handler accepts a WebSocket (RFC 6455) with `websocket::accept()` (`websocket/websocket.hpp`), and has it, as a
`websocket::connection`, until it returns: a WebSocket is a route like any other, so routing, path values and
middleware (e.g. authentication answering 401) run before the handshake.

```cpp
router.GET("/echo",
           [](net::http::server_request& req, net::http::response_writer& resp) -> net::coro::task<void>
           {
               auto conn = co_await net::websocket::accept(req, resp);
               if (!conn) co_return;   // refused: accept() has answered

               std::string message;
               while (true)
               {
                   auto type = co_await conn->read_message(message);
                   if (!type) co_return;   // how it ended: e.g. the client's close, answered already
                   if (co_await conn->write_message(*type, message)) co_return;
               }
           });
```

- **The handshake:** `accept()` checks the request, and answers one that isn't a WebSocket handshake with 400 Bad
  Request (426 Upgrade Required, saying version 13, for another version), returning why (`websocket::errc`, in
  `websocket/error.hpp`); the connection then serves the next request as usual. Otherwise it sends 101 (Switching
  Protocols), with what the handler put in `resp.headers()` (e.g. a `Set-Cookie`), and the connection is the
  WebSocket's. `accept_options::subprotocols` are the server's, most preferred first: the first the client offers is
  chosen (`conn->subprotocol()`). Only HTTP/1.1 so far (a request over HTTP/2 gets 400).
- **Origins:** browsers send cookies with WebSocket handshakes from any site, so a page elsewhere could open one with
  its visitor's cookies (cross-site WebSocket hijacking). A request whose `Origin` isn't the request's own host and
  port is refused (403 Forbidden), unless it matches `accept_options::origins` (e.g. `"*.example.com"`,
  `"localhost:3000"`). A program that isn't a browser sends no `Origin`, and isn't checked. `insecure_any_origin` turns
  the check off, for endpoints that don't authenticate with what browsers send by themselves.
- **Reading:** one task reads at a time (a second read fails with `std::errc::device_or_resource_busy`), and it must
  read, as reads are what answer the client's pings (with a pong) and its close. `read_message(buffer)` replaces a
  `std::string`'s or a `std::vector<std::byte>`'s contents with the next whole message, returning its type (`text` or
  `binary`); nothing's allocated once the buffer's capacity holds it. Or a message is streamed: `next_message()` returns
  its type, then `conn->message()` is an `io::reader` of it, ending (`io::errc::end_of_stream`) at its end. What isn't
  read of a message is skipped by the next read. Text is checked to be UTF-8 as it comes, and a message is limited to
  `accept_options::connection.max_message` (1 MiB), however it's read.
- **Writing:** any number of tasks may write at once, their messages taking turns, never interleaved.
  `write_message(type, data)` sends a message, and returns once it has gone to the connection, so a client that doesn't
  read makes writes wait (until the server's `write_timeout`, which then fails the WebSocket). `begin_message(type)`
  waits for its turn, and returns a `message_writer` (an `io::writer`), each write a frame, ended by `end_message()`,
  for a message too large to hold at once; other messages wait for its end (so a task mustn't write a whole message
  while it has one open), but `ping()` and the replies to the client's pings and close go between its frames.
- **Closing:** `co_await conn->close(code, reason)` sends a close (`close_code::normal_closure` by default), then
  waits for the client's, reading for it itself (and dropping messages that come before it) unless another task is
  reading, for up to `accept_options::connection.close_timeout` (5 s): then it aborts the connection. A connection
  destroyed (e.g. its handler returning) without a close aborts it, so the client sees it end without one. Once the
  handler returns, the server ends the TCP connection, as RFC 6455 has the server do.
- **The server's `close()`:** a WebSocket that's reading is told the server's going away: the read in progress (at
  once if it's waiting, or once it has taken what it has buffered, however busy the client keeps it) or the next read
  sends a close with `close_code::going_away` (1001), reads on for the client's close (dropping the messages not yet
  read, those buffered already too) for up to `close_timeout` (then aborts, however much the client is still sending),
  and returns `websocket::sent(close_code::going_away)`; a `close()` of the handler's own that's under way finishes
  instead. So `server.close()` then `join()` drains WebSockets as it does requests, as long as they read; one that
  isn't reading hears nothing until it next reads (`abort()` ends it whatever it's doing).
- **Idle WebSockets:** while a read waits, `accept_options::connection.idle_timeout` (60 s; 0 turns it off) bounds how
  long the client may say nothing: at half of it, the read pings the client (whose pong, or anything else, starts the
  wait over), and at the end of it, the WebSocket fails with `std::errc::timed_out` and is aborted. The pings also keep
  proxies (e.g. nginx's 60 s `proxy_read_timeout`) from closing a WebSocket that's quiet. Reads do the timing (there's
  no task or timer per WebSocket), so it only applies while a task reads, which it must anyway.
- **Stopping:** `accept_options::stop` ends the WebSocket: whatever waits (a read, a write, `close()`) fails with
  `std::errc::operation_canceled`, and the connection is aborted. By default it's the request's `req.stop`, so the
  server's `abort()` ends a WebSocket, and `http::timeout` on its route bounds the whole WebSocket (not just the
  handshake): put a WebSocket's route outside it, or give `accept()` a token of its own, for one that should outlive the
  route's timeout.
- **How it ended:** an error, as everything else: the client's close, as its code (`err == close_code::going_away`;
  `conn->close_reason()` is its reason), from the read that received it; a close the server sent for what the client
  did wrong (`websocket::sent(close_code::protocol_error)`, `invalid_payload` for text that isn't UTF-8,
  `message_too_big`); `close_code::abnormal_closure` if the connection ended without a close; or the transport's error
  (e.g. `std::errc::timed_out`). `websocket::from_peer(err)` says whose a close was, and `close_code_of(err)` its code.
  The closes that only mean it ended (normal closure, going away, none, abnormal closure) compare equal to
  `io::status_condition::closed`. Once a WebSocket has failed, everything returns the same failure; writing after the
  server's close has gone fails with `std::errc::broken_pipe`.
- **Limits:** a WebSocket holds its HTTP connection, so it counts against `max_connections` until its handler returns,
  `join()` waits for it, and `abort()` ends it (through `req.stop`, its reads and writes failing with
  `std::errc::operation_canceled`; with a `stop` of its own, its reads find the connection's end, `abnormal_closure`). `instrument::http_metrics` and
  `http_tracing` see the 101 as the response's status, and the whole WebSocket as the request's duration.

A client opens a WebSocket with `websocket::connect(client, url, options)`, given an `http::client` whose configuration
it takes (its resolver, `connect_timeout` and `attempt_delay`, its TLS options for `wss://`, `response_header_timeout`
for the handshake, and `io_timeout` for writes), and has the `websocket::connection` until it destroys it:

```cpp
net::http::client client{&scheduler};

auto conn = co_await net::websocket::connect(client, "wss://example.com/chat", {.subprotocols = {"chat.v2"}});
if (!conn) co_return;   // e.g. websocket::errc::bad_status: the server didn't switch

(void)co_await conn->write_message(net::websocket::message_type::text, "hello"sv);

std::string message;
auto type = co_await conn->read_message(message);

(void)co_await conn->close();   // waits for the server's close, then for it to end TCP
```

- **The connection:** `ws://` and `wss://` (and `http://` and `https://`, the same) are spoken over HTTP/1.1, on a
  connection of the WebSocket's own: not one of the client's pool's, nor counted in its limits (over TLS, it offers
  only `http/1.1` with ALPN, so a server that speaks HTTP/2 agrees on HTTP/1.1). `connect_options::headers` go with the
  handshake (e.g. `Authorization`, a `Cookie`), `subprotocols` are offered (`conn->subprotocol()` is the one chosen, or
  empty), and `origin` is sent as `Origin` (programs needn't; browsers do).
- **The handshake fails** (`websocket::errc`) for a response that isn't the server's switching: `bad_status` for
  anything but 101 (Switching Protocols), redirects included (they aren't followed, as browsers don't), `bad_accept`,
  `bad_subprotocol` (one that wasn't offered) or `bad_extension` (none is offered); or as connecting does (e.g.
  `std::errc::timed_out`, a TLS error). `url` and `options` are copied before `connect()` returns.
- **Stopping:** `connect_options::stop` covers connecting, the handshake and the WebSocket, as `accept_options::stop`
  does a server's (`std::errc::operation_canceled`).
- **Reading, writing, limits and how it ended** are as on the server (`connect_options::connection`, the same
  `connection_options`). The client's frames are masked (RFC 6455 has a client mask its frames, with keys a server
  can't predict), into the connection's buffer: the data written isn't changed.
- **Closing:** once the close handshake is done (its own `close()`, or a read of the server's close, answered), the
  client waits for the server to end TCP, as RFC 6455 has the server do, for up to `close_timeout`; then the
  connection ends with its destruction.

`example/websocket-echo` is an echo server (the one Autobahn's TestSuite's `fuzzingclient` tests), and
`example/websocket-autobahn` a client that runs the TestSuite's `fuzzingserver` cases, echoing their messages.

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
- **Client certificates (mutual TLS):** `server_options::client_auth` says whether a server asks for clients'
  certificates (`none`, the default; `optional`, verifying one that's sent; `required`), and `client_ca_file` (needed
  unless `none`, and refused with it) which certificate authorities they must chain to (never the system's); the server
  names them, and a client sends its certificate (`client_options::certificate_chain` and `private_key`, both or
  neither) only if one of its chain's issuers is among them, and the server takes its kind of key. A certificate that
  doesn't verify (an unknown issuer, expired, or without clientAuth in its extended key usage) fails the handshake: the
  server's error is in `verification_category()`, the client's the peer's `bad_certificate`. A client that sends none,
  where one's required, gets `certificate_required` (TLS 1.3) or `handshake_failure` (TLS 1.2). In TLS 1.3, the
  client's handshake is done before the server has looked at its certificate, so it's the client's first read (e.g. of
  the response) that fails, not `connect()`.
- `stream::peer_certificates()` is the chain the peer sent, leaf first (a client's, if it sent one; a server's), as
  `tls::certificate`s (`tls/certificate.hpp`, which builds without TLS too): plain data, made when first asked for,
  then shared (`std::shared_ptr<const std::vector<tls::certificate>>`): the DER, `subject` and `issuer` (as RFC 4514
  writes them, e.g. `CN=alice,O=Example`), `serial_number` (hex), `not_before` and `not_after`, the subject
  alternative names (`dns_names`, `email_addresses`, `uris`, `ip_addresses`), and `sha256` (the DER's, e.g. for
  pinning).
- **Session resumption:** once a TLS 1.3 handshake is done, the server sends the client a session ticket (its session,
  encrypted with the server's key), and the client's next connection to the same host and port resumes the session
  with it: no certificates are sent, signed for or verified (either way), though keys are still exchanged, so a ticket
  key that leaks doesn't decrypt connections recorded before. `stream::resumed()` says whether a handshake did, and a
  resumed session's `peer_certificates()` are those its first handshake verified. A client context keeps up to 1000
  sessions, and offers each ticket once. On the server, it's on unless `server_options::session_tickets` is off;
  tickets are good for `session_ticket_lifetime` (24 hours by default, at most 7 days), and encrypted with a random
  key made with the context and replaced each lifetime (the previous one kept as long again), or with
  `session_ticket_keys` (e.g. shared by servers behind a load balancer, so a client resumes with any of them: the first
  makes tickets, each is tried on them), which `server_context::set_session_ticket_keys()` replaces while the server
  runs. A ticket is bound to the context's client authentication (`client_auth` and `client_ca_file`'s CAs): one made
  where clients were verified differently isn't resumed (a full handshake is made instead), even with the same keys,
  nor is one whose client certificate has expired since. TLS 1.2 sessions aren't resumed (their resumption skips the
  key exchange).
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
| `http/priority.hpp`                    | `http::priority`: reading and writing a `Priority` field (RFC 9218) |
| `websocket/websocket.hpp`             | WebSockets (and `websocket/error.hpp`, which it includes)  |
| `util/linked_stop_source.hpp`          | stop sources that a parent token also stops               |
| `tls/tls.hpp`                          | TLS                                                       |
| `tls/version.hpp`, `tls/certificate.hpp` | `tls::version` and `tls::certificate` (included by `tls/tls.hpp` and `http/request.hpp`; without TLS too) |
| `instrument/...`                       | metrics and tracing                                       |
| `exception.hpp`                        | `gai_category()`                                          |

The rest are what those are built from (e.g. `http/http11.hpp`, the HTTP/1.1 codec; `http/structured_fields.hpp`,
the parser of Structured Fields (RFC 9651) that `http/priority.hpp` reads with; and HTTP/2's, `http/hpack.hpp` and
`http/http2*.hpp`, of which `http/server.hpp` and `http/client.hpp` include only `http/http2_config.hpp`, for their
configurations). `detail` namespaces, and what the headers' comments say is for the scheduler or I/O objects (e.g.
`thread_pool::poller`), aren't for applications.
