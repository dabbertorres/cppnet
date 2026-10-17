# Plan: TLS, with Botan

Status: stage 1 implemented (2026-10-17): `include/tls/tls.hpp`, `src/tls/tls.cpp`, `test/tls/stream.cpp`.

## Goal

HTTPS: first for `http::client` (most APIs a side project calls need it), then for `http::server`. TLS should fit the
coroutine runtime as the sockets do: handshakes and reads are `co_await`ed, bounded by deadlines, canceled by
`scheduler::request_stop()`, and fail with `std::error_condition`s rather than exceptions. Certificates are verified by
default, against the system's trust store.

## Decided

- **Botan 3** (BSD-2, vcpkg's `botan` port, 3.12.0 at the pinned baseline). Its TLS API is driven by buffers and
  callbacks rather than its own I/O, which is what a coroutine runtime needs, and it reads the system's trust store on
  macOS (the keychain) and Linux (`System_Certificate_Store`). The alternatives considered: OpenSSL (more widely
  deployed, but a clunkier API, and verification on macOS needs a certificate bundle found), Mbed TLS (its API is being
  reorganized for 4.0), LibreSSL's `libtls`, BoringSSL (not meant for general use), s2n (Linux-first), wolfSSL (GPL).
- **Botan stays private**, as glaze does: cppnet's API has its own TLS options, and no Botan types.
- **Optional at build time, on by default**: a vcpkg manifest feature (`tls`) and a CMake option (`NET_TLS`, default
  `ON`), so builds that don't need it skip Botan's build (vcpkg's port can't trim Botan's modules: it's the whole
  library). `config.hpp` defines `NET_HAS_TLS` when it's on.
- **Client first.**
- **Each `http::client` has its own `client_context`** (trust store, session cache), not one shared process-wide.
- **The client's handshake times out after 10s by default** (`client_options::tls_handshake_timeout`, separate from
  `connect_timeout`, so an error says which phase it was): a TLS 1.3 handshake is one round trip after TCP's, and 10s
  still finishes one that loses two packets (TCP's retransmission timeout starts at 1s and doubles), while giving up on a
  server that has stopped responding. It's Go's `TLSHandshakeTimeout`, as `http::client`'s other defaults are Go's. The
  server's handshake is part of reading the first request, so `header_read_timeout` bounds it.

## Where things stand

- `http::client` only speaks `http://`. `connection_pool` keys connections by host and port.
- `http::server` only accepts plain TCP; the recommendation so far has been a reverse proxy in front of anything public.
- Everything above the socket is written against `io::reader`/`io::writer` (buffering, limits, chunking, HTTP/1.1), so a
  TLS stream that implements both slots in under it.

## How Botan works

- A `TLS::Client` (or `TLS::Server`) is a state machine: bytes read from the socket go into `received_data()`, and it
  calls back `tls_emit_data()` with bytes to write to the socket, `tls_record_received()` with decrypted application
  data, `tls_alert()`, and `tls_session_established()`. `send()` encrypts application data (emitting it the same way).
  It never does I/O itself.
- Its constructor takes shared callbacks, a session manager (e.g. `Session_Manager_In_Memory`, which is thread-safe, for
  resumption), a `Credentials_Manager` (trusted CAs for a client; the certificate chain and key for a server), a
  `Policy`, an RNG, the server's name (for SNI and verification), and the ALPN protocols to offer.
- Certificate chains are verified by `Callbacks::tls_verify_cert_chain()`, with the trusted roots the credentials
  manager gives. To confirm in stage 1: that the default also checks the server's name against the certificate (a test
  with the wrong name must fail), and which versions the default policy offers (TLS 1.3 should be, with 1.2 allowed).
- A channel isn't documented as thread-safe: cppnet must serialize calls into it.

## Design

### API (names to settle in stage 1)

```cpp
namespace net::tls
{

// client_options configures a client_context.
struct client_options
{
    // Trusted certificate authorities: the system's (default), or those in a PEM file instead (e.g. for a private CA).
    std::optional<std::filesystem::path> trusted_ca_file;

    // Skip verifying the server's certificate (and name): for tests against self-signed servers only. Named so that
    // it's never set by accident.
    bool insecure_skip_verification = false;

    // ALPN protocols to offer (e.g. {"http/1.1"}; later {"h2", "http/1.1"}).
    std::vector<std::string> alpn;
};

// client_context holds what's shared by a client's connections: the trust store (loaded once), the session cache (for
// resumption), the policy and RNG. Cheap to share; http::client keeps one.
class client_context
{
public:
    static std::expected<client_context, std::error_condition> make(const client_options& options);
    ...
};

struct server_options
{
    std::filesystem::path    certificate_chain; // PEM: the server's certificate, then intermediates
    std::filesystem::path    private_key;       // PEM
    std::vector<std::string> alpn = {"http/1.1"};
};

class server_context { ... }; // as client_context

// stream is a TLS connection over a TCP socket, which it owns: an io::reader and io::writer of the application's data.
class stream final : public io::reader, public io::writer
{
public:
    // connect runs the client's handshake over conn, for host (SNI, and the name the certificate must have), until
    // deadline.
    static coro::task<std::expected<stream, std::error_condition>>
    connect(tcp_socket conn, const client_context& ctx, std::string host, io::clock::time_point deadline);

    // accept runs the server's handshake over conn.
    static coro::task<std::expected<stream, std::error_condition>>
    accept(tcp_socket conn, const server_context& ctx, io::clock::time_point deadline);

    coro::task<io::result> read(std::span<std::byte> data) override;   // application data
    coro::task<io::result> write(std::span<const std::byte> data) override;

    // close sends close_notify, and closes the socket.
    coro::task<> close();

    // alpn is the protocol the handshake agreed on, if any.
    [[nodiscard]] std::string_view alpn() const noexcept;

    tcp_socket& socket() noexcept; // e.g. for deadlines and timeouts
};

// tls_category: errors from Botan (alerts, certificate verification) as error_conditions.
const std::error_category& category() noexcept;

}
```

### Inside `stream`

- Ciphertext read from the socket goes into the channel; decrypted records collect in a buffer, which `read()` returns
  from. A `read()` with decrypted data already there finishes without a coroutine (a `ready_task`), as buffered reads
  do; otherwise it reads the socket (the socket's operation), feeds the channel, and repeats until it has data, the
  peer closes, or an error.
- `tls_emit_data()` appends to an outgoing buffer, which is written to the socket after each call into the channel: so
  callbacks never do I/O (they're called inside Botan, under its state).
- A mutex (one per stream) serializes calls into the channel. A read and a write may run at once, on different threads,
  and though TLS's two directions have their own keys, Botan keeps both in one object that isn't documented as
  thread-safe, and receiving can make it send (handshake messages, a TLS 1.3 key update, alerts, close_notify), through
  the same `tls_emit_data()` as `send()`. It's held only for each call (the crypto on what's in memory), never across a
  `co_await` or while waiting on the socket: so different connections never contend, and a stream's reader and writer
  (e.g. HTTP/2's, later; HTTP/1.1 never has both) only take turns on those steps. The alternatives were a stream that
  allows one task at a time (less than a socket does), or one task per stream that does all of Botan's work for the
  others (an extra hop for every read and write).
- The peer closing without close_notify is an error (truncation), except where the HTTP framing already ended the
  message; close_notify is the end of the stream (`status_condition::closed`).
- Deadlines and timeouts are the socket's (`set_read_deadline()`, etc.), so the server's and client's existing limits
  apply unchanged. The handshake has its own deadline.

### HTTP

- **Client:** `https://` URLs connect with `stream::connect()` (port 443 by default). `connection_pool` keys connections
  by scheme as well as host and port, and holds `stream`s or plain sockets. `client_options` gets a `tls::client_options`
  (so tests can trust a test CA). Without `NET_HAS_TLS`, `https://` fails with `not_supported`.
- **Server (stage 3):** `server_config` gets an optional `tls::server_options`; `serve_connection` runs
  `stream::accept()` (under `header_read_timeout`) before reading requests, and serves over the stream.

## Learned in stage 1

- Botan's default verification does check the server's name (and IP addresses, against the certificate's), failing
  with `bad_certificate`; and it offers TLS 1.3 (with 1.2 allowed).
- Botan's default policy requires revocation information (OCSP or CRLs) for the chain, which rejects certificates that
  have none (e.g. Let's Encrypt's: it stopped OCSP in 2025). cppnet's policy doesn't require it, as browsers, Go and
  OpenSSL don't.
- In TLS 1.3, a server may send data (and close_notify) straight after its handshake messages, so they can arrive with
  the end of the client's handshake: the handshake waits for `is_handshake_complete()`, not `is_active()` (which is
  false once the peer has closed), and leaves the rest for `read()`.
- Botan 3.12 doesn't build with libc++ 23 (missing includes): `ports/botan/` patches it.
- macOS's `getaddrinfo()` leaves allocations in the Network framework that LeakSanitizer reports (suppressed in
  `test/sanitizers.cpp`).

## Stages

1. **Dependency, and `tls::stream`.** The vcpkg feature and CMake option; `client_context`, `server_context` and `stream`
   (both sides, as the client's tests need a server); the error category. Tests over loopback, with a CA and
   certificates the tests make with Botan (so nothing expires): handshake and echo both ways; large transfers; ALPN;
   a certificate for another name, and one from an untrusted CA, are rejected; `insecure_skip_verification`; close_notify
   and truncation; a handshake that doesn't finish by its deadline; a read and a write at once (ThreadSanitizer); and,
   like `test/dns.cpp`, one handshake with a real public server (needs the network).
2. **`https://` in `http::client`**, with the pool's keys, and tests against a TLS server in the tests.
3. **TLS in `http::server`**: `server_config`'s options, the handshake in `serve_connection`, and tests (the client from
   stage 2 talking to it).
4. **Later:** HTTP/2 over ALPN (`h2`); client certificates; OCSP; session tickets for the server; trimming Botan's
   build (an overlay port with `--minimized-build`) if it's too slow.
