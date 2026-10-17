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
- **`scheduler.shutdown()`** also waits for the tasks to finish. It's called by the destructor. It can't be called
  from one of the scheduler's own tasks, which would wait for itself: it throws `std::logic_error` there (and the
  destructor terminates).

A server's `close()` stops it accepting and ends its connections first, which is the graceful order:
`server.close()`, then `scheduler.shutdown()`.

Tasks shouldn't block (e.g. on a mutex held for long, or a blocking syscall), as that holds up a worker:
`scheduler.run_blocking(func)` runs a blocking function on threads set aside for that, and returns a task of its
result.

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
- a server outlives `serve()`, and its destructor waits for its connections (so it can't be destroyed from one of
  the scheduler's tasks).

## Errors

Failures are reported, not thrown: functions that can fail return `std::expected<T, std::error_code>`, and reads and
writes return an `io::result{count, err}`. Errors are `std::error_code`s: the OS's (`std::system_category()`), or the
library's own (`io::errc`, `tls::errc`, `gai_category()`'s, ...).

Compare an error against a condition, which matches every error that means the same, rather than a particular code:

```cpp
auto res = co_await socket.read(buf);
if (res.err == std::errc::timed_out) { /* the read's timeout or deadline passed */ }
if (res.err == net::io::status_condition::closed) { /* the stream ended: cleanly, or reset */ }
if (res.err == net::io::errc::end_of_stream) { /* only the clean end of the stream */ }
```

- The end of a stream is `io::errc::end_of_stream`, possibly with the last bytes read. `io::status_condition::closed`
  matches that, the connection being reset or aborted, and TLS's `stream_truncated`.
- Timeouts are `std::errc::timed_out`, and stopping the scheduler `std::errc::operation_canceled`.
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

// a client's side: resolves the host, and connects, within connect_options' timeout (10s).
auto sock = co_await net::tcp_socket::connect(&scheduler, "example.com", "80");
```

A socket's reads and writes can be limited by a **deadline** (a time by which they must finish:
`set_read_deadline()`, `set_write_deadline()`, `set_deadline()`) or a **timeout** (how long each may go without
progress: `set_read_timeout()`, ...), and fail with `std::errc::timed_out` if it passes. `shutdown_write()` ends the
outgoing side (the peer reads the end of the stream). `dns_lookup(host)` (`dns.hpp`) resolves a name's addresses, and
blocks: call it with `run_blocking` from a task.

## HTTP client

`http::client` (`http/client.hpp`) sends HTTP/1.1 requests, to `http://` and `https://` URLs, reusing connections
within `client_options`' limits (per host, altogether, idle time), with timeouts for connecting, the TLS handshake,
the response's head, and stalled reads and writes.

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
first.

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
  `http::static_router` (`http/static_router.hpp`) routes the same way, checked at compile time.
- **A handler** gets the request (as `const`, or not) and a `response_writer`. `resp.send(status, length)` sends the
  head, and returns the writer for a body of that length; `resp.send(status)` is for a body whose length isn't known
  (sent chunked). A handler that sends nothing gets an empty 200; one that throws, a 500.
- **The request's body** is `req.body`, a reader. Whatever the handler doesn't read is skipped.
- **Middleware** wraps a handler: e.g. `instrument::http_metrics{std::move(router)}` (`instrument/middleware.hpp`)
  measures requests with Prometheus metrics, and `instrument::http_tracing` traces them. Middleware passes values to
  handlers in `req.extensions`.
- **`server_config`** sets the host and port, and limits: header and body read timeouts, the write timeout, the size
  of a request's head, and how many connections are served at once. To handle a failure to listen, give the server a
  listener from `listener::make()` instead of a host and port.
- **Stopping:** `server.close()` stops accepting and ends the connections, then `scheduler.shutdown()` waits for
  them.
- **HTTPS:** give `server_config::tls` a `tls::server_context`, made from the certificate chain and private key (PEM
  files). Making it reports a file that can't be loaded as an error, before the server starts:

  ```cpp
  auto ctx = net::tls::server_context::make({.certificate_chain = "chain.pem", .private_key = "key.pem"});
  if (!ctx.has_value()) { /* ctx.error() */ }

  net::http::server server{&scheduler, std::move(*listener), std::move(router), {.tls = std::move(*ctx)}};
  ```

  Each connection's handshake has `header_read_timeout` to finish, and `req.tls` tells a handler its request came
  over TLS. Only HTTP/1.1 is served: a client that agrees on another protocol with ALPN is disconnected.
  `example/http-server` serves HTTPS with `--cert` and `--key`.

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

- The server's certificate is verified against the host name. `client_options::insecure_skip_verification` turns
  that off, for testing.
- Errors compare with `tls::alert` conditions (e.g. `err == tls::alert::bad_certificate`), whichever side sent the
  alert; `tls::from_peer(err)` says which. A certificate that failed verification says why in its message (e.g.
  "Certificate does not match provided name").
- A server makes a `server_context` from its certificate chain and key, and calls `stream::accept()` (as
  `http::server` does).
- `stream::connect()` and `stream::accept()` take an optional `std::stop_token`: requesting stop ends a handshake that's
  waiting, with `std::errc::operation_canceled`.
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
| `dns.hpp`, `ip_addr.hpp`, `url.hpp`    | names, addresses and URLs                                 |
| `http/client.hpp`                      | the HTTP client                                           |
| `http/server.hpp`, `http/router.hpp`, `http/static_router.hpp` | the HTTP server                   |
| `tls/tls.hpp`                          | TLS                                                       |
| `instrument/...`                       | metrics and tracing                                       |
| `exception.hpp`                        | `gai_category()`                                          |

The rest are what those are built from (e.g. `http/http11.hpp`, the HTTP/1.1 codec). `detail` namespaces, and what
the headers' comments say is for the scheduler or I/O objects (e.g. `thread_pool::poller`), aren't for applications.
