# Plan: TLS, with Botan

Status: stages 1 to 3 implemented (2026-10-17): `include/tls/tls.hpp`, `src/tls/tls.cpp`, `test/tls/stream.cpp`;
`https://` in `http::client` (`http/connection_pool.hpp`, `test/tls/client.cpp`); and HTTPS in `http::server`
(`server_config::tls`, `test/tls/server.cpp`). Stage 4 decided (2026-10-20): client certificates, session tickets
for the server, and a trimmed Botan (see "Stage 4"); its questions (at the end) all as recommended. Its first part, a
trimmed Botan, is done (2026-10-20): `ports/botan/portfile.cmake`, the policy in `src/tls/tls.cpp`, and
`test/tls/stream.cpp`'s handshakes per group, cipher suite and kind of certificate (see "Learned in stage 4"). Its
second part, client certificates (mutual TLS), is done (2026-10-20): `tls::client_auth`, `server_options::client_ca_file`,
the client's `certificate_chain` and `private_key`, `tls::certificate` (`include/tls/certificate.hpp`),
`stream::peer_certificates()` and `server_request::peer_certificates`; tests in `test/tls/stream.cpp` and
`test/tls/server.cpp`.

## Goal

HTTPS: first for `http::client` (most APIs a side project calls need it), then for `http::server`. TLS should fit the
coroutine runtime as the sockets do: handshakes and reads are `co_await`ed, bounded by deadlines, canceled by
`scheduler::request_stop()`, and fail with `std::error_code`s rather than exceptions. Certificates are verified by
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
  library). CMake defines `NET_HAS_TLS` (a public compile definition of `net`) when it's on.
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
    static std::expected<client_context, std::error_code> make(const client_options& options);
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
    static coro::task<std::expected<stream, std::error_code>>
    connect(tcp_socket conn, const client_context& ctx, std::string host, io::clock::time_point deadline);

    // accept runs the server's handshake over conn.
    static coro::task<std::expected<stream, std::error_code>>
    accept(tcp_socket conn, const server_context& ctx, io::clock::time_point deadline);

    coro::task<io::result> read(std::span<std::byte> data) override;   // application data
    coro::task<io::result> write(std::span<const std::byte> data) override;

    // close sends close_notify, and closes the socket.
    coro::task<> close();

    // alpn is the protocol the handshake agreed on, if any.
    [[nodiscard]] std::string_view alpn() const noexcept;

    tcp_socket& socket() noexcept; // e.g. for deadlines and timeouts
};

// category: TLS's errors (alerts, either side's) as error codes; verification_category(): why a certificate was
// rejected; alert: conditions matching either side's alerts.
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
  (so tests can trust a test CA). Without `NET_HAS_TLS`, `https://` fails with `protocol_not_supported` (as it does before stage 2).
- **Server (stage 3):** `server_config` gets an optional `tls::server_context` (made with `server_context::make()`, so a
  certificate or key that can't be loaded is an error before the server starts, rather than an exception from its
  constructor); `serve_connection` runs `stream::accept()` (with its own `header_read_timeout` deadline) before reading
  requests, and serves over the stream.

## Learned in stage 1

- Botan's default verification does check the server's name (and IP addresses, against the certificate's), failing
  with `bad_certificate`; and it offers TLS 1.3 (with 1.2 allowed).
- Botan's default policy requires revocation information (OCSP or CRLs) for the chain, which rejects certificates that
  have none (e.g. Let's Encrypt's: it stopped OCSP in 2025). cppnet's policy doesn't require it, as browsers, Go and
  OpenSSL don't.
- Botan's default policy also requires TLS 1.2 peers to support Extended Master Secret, which rejects servers that
  don't (e.g. badssl.com) with only `handshake_failure`. cppnet's policy doesn't, unless `require_extended_master_secret`
  is set, as browsers, Go and OpenSSL don't. (Found by a review that tried the library from its docs.)
- Botan reports a failure as an exception with an alert and text: cppnet's errors say which side sent the alert, and a
  certificate that failed verification is its own code (Botan's `Certificate_Status_Code`, whose text is its message),
  which our `tls_verify_cert_chain` records (doing what Botan's does) before throwing.
- In TLS 1.3, a server may send data (and close_notify) straight after its handshake messages, so they can arrive with
  the end of the client's handshake: the handshake waits for `is_handshake_complete()`, not `is_active()` (which is
  false once the peer has closed), and leaves the rest for `read()`.
- Botan 3.12 doesn't build with libc++ 23 (missing includes): `ports/botan/` patches it.
- macOS's `getaddrinfo()` leaves allocations in the Network framework that LeakSanitizer reports (suppressed in
  `test/sanitizers.cpp`).

## Learned in stage 2

- The pool holds a `detail::connection`: a `tcp_socket` or a `tls::stream`, as a reader and writer; deadlines and
  timeouts are set on its socket either way. Its keys are scheme, host and port, so `http://` and `https://`
  connections to the same port aren't mixed up.
- The pool makes its `tls::client_context` the first time it needs one (`std::call_once`), as loading the system's
  trust store isn't worth it for a client that never uses https; it offers ALPN `http/1.1` (the client's own, rather
  than whatever `client_options::tls` says).
- Checking that an idle connection is still open by peeking at its socket isn't enough for TLS (as first thought, and
  a review found): what the stream has already read from the socket isn't there to peek at. A read takes up to 17 KiB
  of ciphertext, so it can take, with the end of a response, the start of whatever the server sent after it (e.g. an
  unasked-for response, which the next request would read as its own), or its close_notify. So the pool also checks the
  stream itself (`tls::detail::idle_clean()`): nothing decrypted and unread, no part of a record fed to Botan (the
  stream follows the records' framing itself, as `received_data()`'s hint can't say: TLS 1.3's is 5 both between
  records and with 5 bytes of one to come), no close_notify, and no failure.

## Learned in stage 3

- The server serves a `detail::connection` (now in `http/connection.hpp`, shared with the client's pool): a
  `std::variant` of a `tcp_socket` or a `tls::stream` (and without TLS, just a `tcp_socket`), with inline free functions
  for what they do differently (`socket_of()`, `shutdown()`, `close()`). It was a class implementing `io::reader` and `io::writer` by forwarding to the variant; but
  both alternatives already are those, so the buffered reader and writer now use them directly (`as_reader()`,
  `as_writer()`), without the extra virtual call and `std::visit` per read and write. `server_request::tls` tells
  handlers which it is (since replaced by `server_request::tls_version`, the version agreed on, or `none`).
- `server::close()` ended connections by shutting down the reading side of each one's socket, from a set of socket
  pointers. During a TLS handshake, the socket is inside `stream::accept()` (moved to the stream's state), out of the
  server's reach, so `close()` couldn't end one (and `~server` would wait out its deadline, or without
  `header_read_timeout`, until `body_read_timeout` or `write_timeout` passed without progress). `stream::connect()` and `accept()` now take a `std::stop_token`, whose callback shuts the
  socket's reading side during the handshake; and the server's connections use the same (a `std::stop_source` that
  `close()` requests, and a `std::stop_callback` per connection), instead of the mutex-guarded set. Requesting stop
  runs callbacks registered later straight away, so a connection that starts after `close()` ends at once.
- After an error response, the server lingers (draining what the client still sends, so closing doesn't reset the
  connection before the client reads the response). Over TLS it sends close_notify first, without closing the socket:
  `stream::shutdown()`. The drained bytes are read from the socket, undecrypted.
- Botan answers a peer's close_notify with its own (`Callbacks::tls_peer_closed_connection()` returns true by default),
  so after reading the end of the stream, nothing more can be written: TLS 1.3 allows half-closed connections, but
  Botan's default doesn't keep them.
- The server's ALPN protocols come from the `server_context` (whose default is `http/1.1`), so one that offers `h2`
  could agree on it; until HTTP/2 is served, the server closes a connection that agreed on anything but `http/1.1`.
- A plain HTTP request to an HTTPS port fails the handshake (Botan sends an alert, which e.g. curl reports as an
  HTTP/0.9 response); answering it with a plaintext 400, as Go does, was decided against for now.

## Learned in the review

- Writes from several tasks at once (HTTP/2's writer, and a reader's replies) took turns flushing: one that found
  another flushing returned at once, leaving its ciphertext to it. So a write returned before its data had gone, without
  backpressure (what was queued could grow without bound behind a slow peer), its deadline and timeout, or its errors;
  and `shutdown()` could end the socket's outgoing side before close_notify had gone. Now flushing is a queue: a flush
  that finds one in progress waits for it (a `coro::notifier`, resumed on the socket's scheduler's pool) as far as its
  own ciphertext, so the socket's write deadline and timeout limit it as well; the flush in progress writes what's
  queued until nothing is, or hands on to those waiting once its own has gone. A read's replies (e.g. an alert) don't
  wait for a flush in progress, but leave it to write them: a reader that waited for a writer stuck on a peer that's
  itself waiting for its writes to be read would never read them.
- A write that fails may have sent part of a record, after which nothing more can be sent: the failure is kept, and
  every later write (and flush) returns it.
- A read into an empty span returns at once (as `io::reader` says), rather than waiting for data.
- `server_context::make()` fails (`invalid_argument`) if the key isn't the certificate's (comparing their public keys,
  as Botan encodes them), rather than every handshake failing.

## Stage 4

Three things, chosen for stage 4: client certificates, session resumption for the server, and a smaller Botan. What
Botan does is from its 3.12 sources (`vcpkg/buildtrees/botan/src/`), and Go's from `crypto/tls` (its sources as of
October 2026).

### Client certificates (mutual TLS)

**Where things stand.** Neither side can use them. Botan's server asks for a client certificate when the credentials
manager's `trusted_certificate_authorities("tls-server", sni)` has any CAs, or the policy's
`request_client_certificate_authentication()` is true: `server_credentials` doesn't override the first (Botan's default
returns none) and the policy the second, so no CertificateRequest is ever sent. `client_credentials` gives only trusted
CAs, so a client asked for one sends an empty Certificate. `callbacks::tls_verify_cert_chain()` already handles a
client's chain (`Usage_Type::TLS_CLIENT_AUTH`, no host name), and nothing reads the peer's chain after the handshake.

**How Botan does it.**

- The server: a CertificateRequest goes out if the `"tls-server"` stores have subjects, or the policy requests one,
  listing those subjects as the acceptable CAs (TLS 1.3's `certificate_authorities` extension; TLS 1.2's list). An empty
  Certificate fails the handshake if `require_client_certificate_authentication()` (TLS 1.3 with `certificate_required`,
  116; TLS 1.2 with `handshake_failure`), and is otherwise accepted. A certificate that is sent goes to
  `tls_verify_cert_chain()` with the `"tls-server"` stores and `TLS_CLIENT_AUTH` (Botan's path validation then checks
  the leaf's key usage, and that its extended key usage, if it has one, includes clientAuth); a failure is
  `bad_certificate`, required or not (RFC 8446 4.4.2.4 lets a server go on unauthenticated instead; Botan doesn't,
  unless the callback ignores the failure). CertificateVerify proves the client has the key.
- The client: `find_cert_chain(key_types, cert_signature_schemes, acceptable_CAs, "tls-client", host)` and
  `private_key_for(leaf, ...)`, as the server's credentials already do for `"tls-server"`; an empty chain sends an empty
  Certificate.
- After the handshake, `Channel::peer_cert_chain()` is the peer's chain, leaf first, also after a resumption (the
  session carries it: `Session_Base::peer_certs()`). Botan doesn't check, on resuming, that the client's certificate is
  still valid, or that one is still required.
- In TLS 1.3, the client's handshake is complete once it has sent its Finished, before the server has looked at its
  certificate: a rejection reaches the client as an alert on its first read, not from `connect()`. In TLS 1.2 the server
  checks it before its own Finished, so `connect()` fails.

**How Go does it.** `Config.ClientAuth` is one of `NoClientCert`, `RequestClientCert`, `RequireAnyClientCert` (neither
verifies what's sent), `VerifyClientCertIfGiven` and `RequireAndVerifyClientCert`; `ClientCAs` are the roots it verifies
against (nil means the system's, a known trap) and whose subjects it sends. A client has `Certificates` (sending the
first that `CertificateRequestInfo.SupportsCertificate` accepts: a key the server can verify, issued by one of its
acceptable CAs if it listed any; otherwise an empty Certificate) or a `GetClientCertificate` callback.
`VerifyPeerCertificate` and `VerifyConnection` are hooks for checks of one's own (the only use of the unverifying
modes). `ConnectionState` has `PeerCertificates` and `VerifiedChains`, and `net/http` gives handlers `Request.TLS`.

**Proposed API.**

```cpp
namespace net::tls
{

// tls/certificate.hpp: needs neither Botan nor NET_HAS_TLS (as tls/version.hpp), so http/request.hpp can include it.
// certificate is an X.509 certificate a peer sent, as data: its DER, and what's commonly checked, read from it.
struct certificate
{
    std::vector<std::byte>                der{};
    std::string                           subject{};       // RFC 4514, e.g. "CN=alice,O=Example"
    std::string                           issuer{};
    std::string                           serial_number{}; // hex
    std::chrono::system_clock::time_point not_before{};
    std::chrono::system_clock::time_point not_after{};
    // the subject alternative name's entries (e.g. a SPIFFE ID among uris)
    std::vector<std::string>              dns_names{};
    std::vector<std::string>              email_addresses{};
    std::vector<std::string>              uris{};
    std::vector<ip_addr>                  ip_addresses{};
    std::array<std::byte, 32>             sha256{};        // of der: e.g. for pinning
};

// client_auth is whether a server asks for clients' certificates (Go's ClientAuth, without its unverifying modes).
enum class client_auth : std::uint8_t
{
    none,     // not asked for (NoClientCert)
    optional, // asked for; one that's sent must verify (VerifyClientCertIfGiven)
    required, // one that verifies is required (RequireAndVerifyClientCert)
};

struct server_options
{
    // ... as now, and:
    client_auth                          client_auth = client_auth::none;
    // PEM: the certificate authorities clients' certificates must chain to (and whose names the server sends). Needed
    // unless client_auth is none (and refused then).
    std::optional<std::filesystem::path> client_ca_file{};
};

struct client_options
{
    // ... as now, and:
    // PEM files: a certificate (and intermediates) and its key, sent to a server that asks for one and would accept
    // it (its kind of key, and issued by one of the CAs it names, if it names any). Both, or neither.
    std::optional<std::filesystem::path> certificate_chain{};
    std::optional<std::filesystem::path> private_key{};
};

class stream
{
    // ... as now, and:
    // peer_certificates is the chain the peer sent (a server's, or a client's if it sent one), leaf first: empty if
    // none. Made the first time it's asked for, then shared (so copies are cheap and outlive the stream).
    [[nodiscard]] std::shared_ptr<const std::vector<certificate>> peer_certificates() const;
};

}
```

- `make()` fails with `invalid_argument` for `client_auth` other than `none` without `client_ca_file` (or `none` with
  one), a client certificate without its key (or a key without one), and a client key that isn't the certificate's (the
  check the server's already has); with the file's error for one that can't be read.
- Errors are the existing ones: a server that requires a certificate and gets none fails with `sent_alert_base + 116`
  (TLS 1.3) or `+ 40` (TLS 1.2), and its client with the peer's alert; one that doesn't verify, on the server with
  `verification_category()`'s code (e.g. an unknown issuer, expired, or the wrong extended key usage), and on the client
  with the peer's `bad_certificate`. All compare equal to `tls::alert` conditions as now.

**Inside.** `client_credentials` and `server_credentials` become one `credentials` class: the store it trusts for the
peer's certificates (for `"tls-client"`: the server's CAs, as now; for `"tls-server"`: the client CAs, returned only
when `client_auth` isn't `none`, so nothing is requested otherwise), and its own chain and key, if any, for its own
side. `find_cert_chain()` does what Go's `SupportsCertificate` does: the leaf's key type among those asked for, and if
CAs are named, one of the chain's issuers among them. The policy overrides `request_client_certificate_authentication()`
and `require_client_certificate_authentication()` from `client_auth`. The stream keeps Botan's chain (copies of
`X509_Certificate` share their data) once the handshake is complete, and makes `tls::certificate`s from it the first
time `peer_certificates()` is called (under the stream's mutex), so a connection that never asks pays nothing; `subject`
and `issuer` are formatted by cppnet (Botan's `X509_DN::to_string()` isn't RFC 4514), and SAN entries come from Botan's
sets (sorted, not in the certificate's order).

**HTTP.** `server_request::peer_certificates` (a `std::shared_ptr<const std::vector<tls::certificate>>`, null without
one) is set where `tls_version` is (once per HTTP/1.1 connection, kept by `clear()`; per HTTP/2 stream, a copy of the
connection's), and only if the server's context asks for client certificates, so other servers don't make them. A
handler (or middleware) authorizes from it, e.g. answering 403 for a subject it doesn't know. `http::client` gets client
certificates through `client_options::tls`, as its trusted CAs; the pool's keys don't change (a client has one
identity). In TLS 1.3, a rejected certificate fails the request when its response is read (HTTP/1.1) or the connection's
first read (HTTP/2), with the peer's alert; it's a new connection, so it isn't retried.

**Costs.** None without client authentication (one branch in the credentials manager). With it: the client's chain
verified per full handshake (as a client verifies a server's), the CertificateRequest's CA names (as large as the client
CA file's subjects), and `tls::certificate`s made per connection a handler might look at.

**Tests** (`test/tls/stream.cpp`, `test/tls/server.cpp`, `test/tls/client.cpp`; no network). `test_pki` gets a client
CA, and from it a client certificate (extended key usage clientAuth), one with only serverAuth, one expired, and one
from an untrusted CA. For TLS 1.2 and 1.3: `none` sends no request (a client with a certificate sends nothing, and the
server sees no peer certificates); `optional` without a certificate succeeds, with a good one the server sees it (DER,
subject, SANs, fingerprint), with a bad one (untrusted, expired, wrong usage) fails (the server with the verification
code, the client with the alert: from `connect()` in TLS 1.2, from the first read in 1.3); `required` without one fails
with `certificate_required` (1.3) or `handshake_failure` (1.2); a client whose certificate's issuer isn't among the
named CAs sends none; the client sees the server's chain; `make()`'s errors. Over HTTP: a handler sees the client's
certificate over HTTP/1.1 and HTTP/2, and none in cleartext or without client authentication; `http::client` with a
certificate; a rejected one failing `get()`.

### Session resumption for the server

**Where things stand.** The server's session manager is `Session_Manager_Noop`: no TLS 1.2 session IDs or tickets, no
TLS 1.3 tickets, so every connection is a full handshake. The client's is `Session_Manager_In_Memory` (Botan's default
size, 1000), so it already resumes with servers that issue tickets (and, per Botan's policy, uses each ticket once, as
RFC 8446 C.4 recommends). It's keyed by `Server_Information{host}`, without the port: a ticket from one port is offered
to another, where it's ignored (a full handshake) and used up.

**How Botan does it.**

- Session managers: `Session_Manager_In_Memory` (stateful: sessions in a map, a TLS 1.2 session ID or a TLS 1.3 ticket
  that's only a handle), `Session_Manager_Stateless` (the session encrypted into the ticket, with a key from
  `Credentials_Manager::session_ticket_key()`: a single key, so no rotation), `Session_Manager_Hybrid` (both), and
  `Session_Manager_Noop`. The base class is public and meant to be derived from (`establish`, `retrieve_one`, `store`,
  `remove`, `find_some`, `emits_session_tickets`). `Session::encrypt(key, rng)` and `Session::decrypt(ticket, key)` are
  public: HMAC(SHA-512-256) of the key derives a 4-byte key name, checked first, and a per-ticket AES-256/GCM key from a
  random seed. The ticket carries the whole session, the peer's certificates included.
- `retrieve()` drops sessions older than `Policy::session_ticket_lifetime()` (one day by default; TLS 1.3's at most
  seven). A TLS 1.3 server sends `new_session_tickets_upon_handshake_success()` tickets (one by default) once the
  client's Finished is in, and resumes only on the same SNI and a cipher suite with the same hash; it only implements
  `psk_dhe_ke`, so a resumption still does an ECDHE exchange (forward secrecy kept). A TLS 1.2 resumption is the
  abbreviated handshake (one round trip less, no key exchange: the ticket key protects those sessions' secrets).
- `Session_Summary::was_resumption()` (in `tls_session_established()`) says whether a handshake resumed.

**How Go does it.** Tickets are on by default (`SessionTicketsDisabled`), for TLS 1.2 and 1.3, one per TLS 1.3
connection, valid 7 days. Without keys of its own, a server's key is random and rotated every 24 hours, older keys kept
for 7 days to decrypt (`ticketKeyRotation`, `ticketKeyLifetime`, done lazily as tickets are made);
`SetSessionTicketKeys` sets keys (the first encrypts, all decrypt) and stops the rotation, so a fleet can share them.
Resuming a TLS 1.3 ticket checks its age, the suite's hash, that a session without a client certificate isn't resumed
where one is required, that the client's leaf hasn't expired, and (since CVE-2025-68121, Go 1.25.7, February 2026: a
resumption that succeeded after `ClientCAs` or `RootCAs` had changed) that the session's verified chains still verify
against the current configuration. A Go client resumes only with a `ClientSessionCache` (none by default).

**Proposed API.**

```cpp
namespace net::tls
{

using session_ticket_key = std::array<std::byte, 32>;

struct server_options
{
    // ... as above, and:
    // Session resumption with tickets: on by default, for TLS 1.3 (Decided question 8). A ticket is good for
    // session_ticket_lifetime (at most 7 days, TLS 1.3's limit), and a resumption for as long.
    bool                 session_tickets         = true;
    std::chrono::seconds session_ticket_lifetime = std::chrono::hours{24};
    // The keys that protect tickets (e.g. shared by a fleet of servers): the first makes them, all are tried on them.
    // Empty: random keys, made when the context is, and rotated every session_ticket_lifetime.
    std::vector<session_ticket_key> session_ticket_keys{};
};

class server_context
{
    // ... as now, and:
    // set_session_ticket_keys replaces the keys (as session_ticket_keys), from any thread, while connections go on;
    // automatic rotation stops. Empty turns tickets off.
    void set_session_ticket_keys(std::vector<session_ticket_key> keys);
};

class stream
{
    // resumed is whether the handshake resumed an earlier session (Go's DidResume).
    [[nodiscard]] bool resumed() const noexcept;
};

}
```

`make()` fails with `invalid_argument` for a lifetime of 0 or over 7 days.

**Inside.** A session manager of cppnet's (`ticket_manager`, deriving from `Botan::TLS::Session_Manager`), in the
server's state:

- `establish(session, id, tls12_no_ticket)` returns a ticket, `session.encrypt(current key)`, for a TLS 1.3 session (and
  none for TLS 1.2's, nor session IDs: Decided question 8); `retrieve_one(handle)` decrypts with each key in turn (the
  key name rejects the others before any decryption); `find_some()` is empty (it's a server's), and `store()`/`remove()`
  do nothing. `emits_session_tickets()` is whether there's a key.
- Keys: the given ones, or a random one made with the context, and, when it's a lifetime old, another made in its place
  (lazily, under the manager's mutex, as Go does, so there's no timer), the previous one kept for a lifetime more: every
  ticket within its lifetime can still be decrypted, and a key is gone two lifetimes after it began.
- The key Botan uses is derived from the configured one and the context's client authentication: HMAC-SHA-256 over
  `client_auth` and a hash of the client CAs' certificates. So a ticket made where client certificates were verified
  against other CAs (another context sharing the keys, or a restart with a new CA file) doesn't decrypt, which is what
  Go's re-verification guards against, without verifying the chain again on each resumption (Decided question 10). Then
  `retrieve_one()` refuses what Go refuses: a session whose client leaf has expired, or without a client certificate
  where one is required.
- The policy's `session_ticket_lifetime()` comes from the options; the client's policy keeps Botan's default.
- `tls_session_established()` records `was_resumption()` for `resumed()`.
- The client's sessions are keyed by host and port (`Server_Information{host, port}`, the port being the socket's
  peer's), so a ticket is offered only where it came from.

**HTTP.** Nothing in `http::server`'s API: `server_config::tls`'s context carries it. `http::client` already resumes
(with its own context's cache); against cppnet's server it now will.

**Costs.** Per full TLS 1.3 handshake, one ticket made (an AES-GCM encryption and a few HMACs, of a session of a few
hundred bytes, more with a client's chain) and sent (in the flight after the handshake, which the reader writes as it
writes other replies), to clients that resume and to those that never will (e.g. a one-shot curl). Per resumption:
decrypting it, and no certificate sent, signed for, or verified (the server's signature, the client's chain
verification, and with client certificates, the server's verification of theirs), though TLS 1.3 still does its key
exchange and takes the same round trips. No memory per session (it's in the ticket). A benchmark (`bench_tls`: full and
resumed handshakes over loopback, ECDSA P-256 and RSA 2048 server keys) measures it, for a note here.

**Tests** (no network; a clock in the server's state, an internal hook defaulting to the system's, as the resolver's
`facts` are, passed through `Callbacks::tls_current_timestamp()` and the rotation). TLS 1.3: a second connection with
the same client context resumes (`resumed()` on both sides) and the handler sees the same client certificate; tickets
off, never; a ticket from another context (another random key) is a full handshake; shared keys across two contexts
resume; `set_session_ticket_keys()` with the old key second still resumes, without it doesn't; the clock past the
lifetime, a full handshake; rotation (the clock a lifetime on: still resumes with an earlier ticket, two lifetimes on:
doesn't); contexts with the same keys but other client CAs don't resume; a client leaf that has expired since isn't
resumed (and then fails verification); a ticket for one SNI isn't resumed on another. TLS 1.2: no ticket, a full
handshake each time. The client: a ticket from one port isn't offered to another.

### Trimming Botan

**Where things stand.** vcpkg's port (and so the overlay) builds all of Botan: 556 library sources here, plus Botan's
command-line tool (45 more, installed by `vcpkg_copy_tools()`, unused by cppnet). On this machine (GCC 14, x86-64, 16
threads) the port took about 3m45s for its debug build and 4m25s for its release build (the build logs' times); the
release library is 19 MB, the debug one 470 MB. CI pays it whenever its cache key changes (the vcpkg commit, compiler
version, `vcpkg.json` or `ports/`).

**How Botan does it.** `configure.py --minimized-build` loads only what's always loaded (`utils`, ...) and what
`--enable-modules=` names, with their dependencies; modules for particular CPUs (AES-NI, CLMUL, SHA extensions, AVX2,
ARMv8's) are separate modules not loaded unless named, and names may use `*` (`aes_*`), where those for other CPUs are
skipped (`Skipping (incompatible CPU)`). A minimized build has no RNG unless one is named (`system_rng`). Botan's own
module policies (`src/build-data/policy/modern.txt`) are a reference, not a fit (they keep much more).

**What cppnet uses.** TLS 1.2 and 1.3 (`tls12`, `tls13`), with the PQ hybrid groups (`tls13_pqc`, `ml_kem`:
X25519MLKEM768, Go's default since 1.24, and what browsers offer); key exchange `x25519`, `ecdh` on P-256, P-384 and
P-521 (`pcurves_secp256r1`, `_secp384r1`, `_secp521r1`); certificates `ecdsa` and `rsa` (`emsa_pkcs1`, `emsa_pssr`,
`eme_pkcs1` for TLS 1.2); `aes`, `gcm`, `chacha20poly1305`; `sha2_32`, `sha2_64` (SHA-512-256 for tickets), `hmac`,
`hkdf`, `prf_tls`; `x509` (path validation, and for the tests `X509_CA` and PKCS #10), `pem`, `certstor_system` (with
`certstor_system_macos` on Apple's platforms with Clang, and `certstor_flatfile` on Linux, its bundle found by
`configure.py`); `system_rng`; and the CPUs' versions: `aes_*`, `ghash_*`, `sha1_*`, `sha2_32_*`, `sha2_64_*`,
`chacha_*`, `poly1305_*` (and `keccak_perm_*` for ML-KEM). Botan's default policy also offers X448, the brainpool curves
and FFDHE (and DHE suites in TLS 1.2), which Go doesn't: the policy drops them (`key_exchange_groups()`,
`allowed_key_exchange_methods()` returning only ECDH), so what's built is what's offered, whichever Botan cppnet is
built against (a project with its own vcpkg manifest gets vcpkg's whole Botan, without the overlay). Botan's default
ciphers are already only AEADs (no `tls_cbc`), and its signature hashes SHA-256 to 512.

**Measured** (configured as above with `configure.py` from the port's sources; the library only, release, GCC 14 `-O3`,
`make -j16`, on this machine): 251 sources instead of 556; 98 s instead of 206 s; 9.0 MB instead of 19.2 MB. Without the
tool too, the port's two builds should take under half as long as now.

**Changes.** `ports/botan/portfile.cmake`: `--minimized-build --enable-modules=...` (one list, with a comment per
group), `--build-targets=static` (or `shared`), not `cli`, and no `vcpkg_copy_tools()`; GCC on macOS keeps disabling
`certstor_system_macos` and `sm3_armv8` (the latter then not loaded anyway). (vcpkg's ABI hash covers the portfile, so
caches build it again once.) `src/tls/tls.cpp`'s policy lists the groups and key exchanges. The README's build time
note.

**Tests.** The suite (which makes certificates with Botan, so covers the tests' needs: ECDSA P-256 keys, CAs, PKCS #8),
on every CI toolchain; and one handshake per group the policy lists, between cppnet's client and server, the client's
offer limited to that group (an internal policy hook), so a group whose module is missing fails a test rather than a
handshake in the field. The network tests (public servers) exercise interoperability as now.

## Learned in stage 4

### A trimmed Botan (part 1)

- **The module list** is in `ports/botan/portfile.cmake` (`cppnet_modules`, a comment per group), as planned, with
  three more than the plan named. `rfc6979`: Botan's ECDSA derives its nonces as RFC 6979 does (hedged with the RNG) only
  when that module is built, and otherwise takes them from the RNG alone, so without it a server's ECDSA signatures
  would depend on the RNG more than a whole Botan's do. `ctr_*`: GCM runs AES in CTR mode, whose AVX2 and SIMD versions
  are CPU-specific modules like the others. And `certstor_flatfile` everywhere (on Apple's platforms with Clang too,
  where the keychain is used instead), as GCC on macOS needs it; `certstor_system_macos` is named everywhere too, and
  `configure.py` skips it on other systems (as it skips other CPUs' modules), so the list has no conditions. `chacha_*`
  also names ChaCha's RNG, which isn't a CPU's version: it's disabled (`--disable-modules`, which wins over
  `--enable-modules`, as the GCC-on-macOS exclusions already relied on).
- **What comes along** (with dependencies, as `configure.py` resolves them): `tls` requires `dh`, so finite field
  Diffie-Hellman is still built, though the policy no longer offers it; `x509` requires `sha1` (key identifiers, OCSP);
  `rfc6979` brings `hmac_drbg`; `ml_kem` brings SHA-3 and SHAKE. 257 sources are built (x86-64 macOS, Clang).
- **Left out, as not used** (each loaded by a whole build): `thread_utils`, so Botan's RSA private key operations run
  on the calling thread rather than splitting across Botan's own thread pool (only a server with an RSA key does them;
  cppnet's tasks shouldn't block anyway); `locking_allocator`, so Botan's secure memory is zeroed when freed but not
  `mlock()`ed (as Go's and OpenSSL's defaults); `auto_rng`, `entropy` and the CPUs' RNGs, as cppnet gives Botan
  `System_RNG`; `http_util`, so no online OCSP checks (cppnet never asked for them); `pbes2`, so an encrypted PKCS #8
  key can't be read (it couldn't before either: `server_options` takes no passphrase); and Ed25519/Ed448, which Botan's
  default policy doesn't allow for certificates anyway.
- **The policy** (`src/tls/tls.cpp`) offers the groups decided (X25519, P-256, the three ML-KEM hybrids, P-384, P-521,
  in Botan's order), of those the Botan it's built against has (`Group_Params::is_available()`), and only ECDH as TLS
  1.2's key exchange (no DHE suites). Unchanged: Botan's ciphers (already only AES-GCM and ChaCha20-Poly1305), its
  signature methods and hashes, and its client's offer of a key share for X25519 only (the hybrids listed, so a server
  that prefers one asks for it with a HelloRetryRequest). Against servers that speak only DHE (e.g. `dh2048.badssl.com`)
  handshakes now fail, as Go's do.
- **The tests** narrow what the client offers through an internal hook, `tls::detail::make_client_context(options,
  policy_limits)` (groups and cipher suites by IANA code point; each narrows the policy's, so one the policy doesn't
  offer leaves nothing, and fails too, as checked by hand with X448, brainpool, FFDHE and TLS_AES_128_CCM_SHA256).
  `test/tls/stream.cpp` has a handshake per group over TLS 1.3 (the seven) and 1.2 (the four that aren't hybrids), per
  cipher suite (TLS 1.3's three, and TLS 1.2's three for each of ECDSA and RSA), and with an RSA certificate over TLS 1.3;
  `test/tls/test_pki.hpp` gained an RSA CA (trusted, in `ca.pem` beside the ECDSA one) and an RSA certificate for
  localhost (2048 bits; made in well under a second, even in a debug build). A network test has `cloudflare.com` and
  `www.google.com` agree on X25519MLKEM768 alone, against other implementations than Botan's. By hand (not kept, as
  public servers change): 12 public servers and badssl.com's variants (RSA 2048 and 4096, ECDSA P-256 and P-384,
  SHA-256 chains, TLS 1.2 only) handshake; those that fail fail as a whole Botan would (expired certificates, a chain
  whose SHA-1 intermediate isn't sent) or are DHE-only; every group was agreed on by some server (P-256 with ML-KEM by Amazon's; P-384
  with ML-KEM-1024 by none, so only the local test covers it).
- **Measured** (this machine, Homebrew Clang 23, x86-64, the port's own builds as vcpkg ran them): Botan's debug build
  1m46s and release build 2m11s, 4.4 minutes for the whole install (with its download, patches and configures), where
  the plan measured 3m45s and 4m25s with GCC 14 for the whole library and the tool; the libraries are 8.8 MB (release)
  and 183 MB (debug), from 17 MB and 317 MB as Clang built them whole. No `tools/botan` is installed.
- vcpkg's ABI hash covers the portfile, so every cache (CI's too) builds Botan again once. CI's GCC, Linux, arm64 and
  libstdc++ builds are the check of the list on other CPUs, compilers and trust stores; `configure.py` was run by hand
  for macOS and Linux on x86-64 and arm64 (with `--disable-cc-tests`), loading the same modules but the CPUs'.

### Client certificates (part 2)

- **As proposed:** the API (`tls::client_auth`'s three modes, `server_options::client_auth` and `client_ca_file`,
  `client_options::certificate_chain` and `private_key`, `tls::certificate`, `stream::peer_certificates()`,
  `server_request::peer_certificates`), one `credentials` manager for both sides (a side's type, `"tls-client"` or
  `"tls-server"`, naming both the CAs it trusts for the peer and its own chain; a server's CAs are null unless it asks,
  so nothing is requested otherwise), the policy's `request_`/`require_client_certificate_authentication()` from
  `client_auth`, and `make()`'s errors. `tls::certificate` is in `include/tls/certificate.hpp`, which needs only
  `ip_addr.hpp`, so `http/request.hpp` includes it in every build. No Botan module was added: the DER, names and SANs
  are `x509`'s (`BER_Decoder`, `AlternativeName`), SHA-256 is `sha2_32`'s (looked up by name, as `HashFunction`), and
  hex is cppnet's own (`encoding/hex.hpp`, so upper case). The tests' certificates (`DER_Encoder`,
  `X509_CA::make_cert()`) are `x509`'s too.
- **Choosing the client's certificate** (`find_cert_chain()`, Go's `SupportsCertificate`) holds only a client's to the
  CAs the server names (any of its chain's issuers among them, by `X509_DN`'s comparison), not a server's to a client's
  `certificate_authorities` extension (rare), as Go's server doesn't either (it has one certificate). So a client whose
  certificate is from a CA the server doesn't name sends none: with `optional` it's served unauthenticated, with
  `required` it gets `certificate_required`. The tests' "untrusted" client certificate is therefore an impostor's: from
  a CA with the client CA's name but another key, which the client sends and the server can't verify ("Certificate
  issuer not found"); one from a CA with another name is the test that nothing is sent.
- **The peer's chain** is kept (`channel->peer_cert_chain()`, copies sharing Botan's data) when the handshake is
  complete; `peer_certificates()` makes the `tls::certificate`s the first time (under the stream's mutex) and keeps
  them. It never returns null: no chain is an empty vector, a function-local static shared through an aliasing
  `shared_ptr` without an owner, so asking costs no allocation. The HTTP server sets `server_request::peer_certificates`
  from it when it isn't empty, and null otherwise (`detail::peer_certificates(conn)`, in `http/connection.hpp`): the
  plan's "only if the server's context asks" is that, as a client only sends a certificate when asked, so no check of
  the context is needed, and a server that doesn't ask makes nothing. HTTP/2 makes it once per connection (in the
  connection's constructor), each stream's request sharing it.
- **`subject` and `issuer`** are formatted from the name's DER (`raw_subject_dn()`), as Botan's `X509_DN` flattens
  multi-valued RDNs: RDNs last first, joined by `,`, a multi-valued RDN's attributes by `+` (in their DER order); RFC
  4514's names for its nine types, other types as their OID with the value's DER in hex (`2.5.4.5=#13023432`, as RFC
  4514 2.4 says, and Go does); values escaped as 2.4 says, plus control characters as `\XX` (which it allows, so a name
  can't break a log line). The tests' client certificate has each case (made from DER, as Botan's `X509_DN` can't make
  a multi-valued RDN). `serial_number` is the number's bytes in hex (Botan keeps it as a `BigInt`'s, without leading
  zero bytes; "00" for 0). `not_before` and `not_after` are read from the time's fields (Botan's `to_std_timepoint()`
  fails for times before 1970, or that `time_t` can't hold), and clamped to the clock's range (libstdc++'s
  `system_clock` ends in 2262, before RFC 5280's 9999-12-31).
- **Where a refusal surfaces**, as the plan said: over TLS 1.2, `connect()` fails (the server's Finished comes after
  its look at the certificate); over TLS 1.3, `connect()` succeeds and the first read fails with the server's alert
  (`test/tls/stream.cpp` checks both, for each mode and kind of certificate). Over HTTP/1.1 the request is written,
  then reading its response fails with the alert (a reset after it comes behind the alert, which is read first). Over
  HTTP/2, the connection's end failed every stream with `connection_aborted`, which also counted as a dropped
  connection, so the request was sent again on a new one (and failed the same way). Now a connection whose read fails
  with a TLS alert (`detail::tls_failure()`: TLS's category, but not `stream_truncated`) fails its streams with the
  alert (`end(why)`), and they aren't retried. And a write that fails because the connection was closed (a reset, or a
  broken pipe: `status_condition::closed`) no longer ends it itself: the reader finds the end at once, and the alert
  before it, if the peer sent one; otherwise the HTTP/2 client could write (e.g. its HEADERS after its preface) into
  the server's reset, ending the streams with `connection_aborted` before the reader had read the alert. Other write
  failures (e.g. a timeout) still end the connection at once.
- **Session resumption (part 3)** isn't touched: the server's session manager is still `Session_Manager_Noop`, so every
  handshake is full, and the chain is the one just verified. On a resumption, Botan's `peer_cert_chain()` is the
  session's, so `peer_certificates()` will be the chain verified when the session began; and a TLS 1.3 server sends no
  CertificateRequest in a resumed (PSK) handshake. Part 3 decides which sessions may resume (Decided question 10).

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
4. **Decided (2026-10-20), in three pull requests, in this order** (see "Stage 4"):
   1. **A trimmed Botan. Done (2026-10-20).** The overlay port configures `--minimized-build` with the modules cppnet uses (Decided
      questions 11 and 12), and stops building Botan's command-line tool; the policy offers only what's built (the
      key exchange groups, Go's set), so cppnet behaves the same over a whole Botan (e.g. a project's own vcpkg's).
      First, as it's independent, and halves the dependency's build for every CI run after it (once its cache is
      made again).
   2. **Client certificates (mutual TLS). Done (2026-10-20).** `server_options::client_auth` and `client_ca_file`; a certificate and key
      in `client_options` (so `http::client`'s too); `tls::certificate` (Botan-free), `stream::peer_certificates()`
      and `server_request::peer_certificates`; one credentials manager for both sides; client certificates in the
      tests' PKI.
   3. **Session resumption for the server.** Session tickets, from a session manager of cppnet's (keys rotated, or
      given; bound to the context's client authentication), `stream::resumed()`, the client's session cache keyed by
      port as well, and a benchmark of full and resumed handshakes. After client certificates, as which sessions may
      be resumed depends on them.

   HTTP/2 over ALPN (`h2`), listed here before, is done: the server's in the HTTP/2 plan's stage 3, the client's in its
   stage 4. OCSP is dropped (see "Out of scope").

## Decided questions (stage 4)

All decided as recommended (2026-10-20); the options are kept for the reasoning.

1. **Which client authentication modes.** (a) Go's five (with `request` and `require_any`, which don't verify what's
   sent); (b) three that verify: `none`, `optional`, `required`; (c) two: `none` and `required`. **Recommended: (b)**:
   an unverified certificate means something only to a check of one's own (Go's `VerifyPeerCertificate`), which cppnet
   doesn't have; a handler could check a pinned fingerprint, but a mode named "require" that accepts anyone is the trap
   Go's has. They can come with a verification hook, if something needs one.
2. **Where client CAs come from.** (a) A PEM file, required unless `none` (and refused with `none`); (b) the same, but
   the system's trust store without one (Go's nil `ClientCAs`); (c) a list of certificates in memory as well as the
   file. **Recommended: (a)**: client certificates are almost always from a private CA, and trusting every public CA for
   them lets anyone with a web certificate in; (c) can come when contexts take certificates from memory (as
   `trusted_ca_file` would too).
3. **How a handler sees the client's certificate.** (a) `server_request::peer_certificates`, a shared pointer to the
   chain, beside `tls_version`; (b) a `tls::connection_state` (Go's `ConnectionState`: version, ALPN, resumed, server
   name, peer certificates) as `server_request::tls`, replacing `tls_version`; (c) only `stream::peer_certificates()`,
   for code below HTTP. **Recommended: (a)**: what handlers need, made only for servers that ask for certificates;
   `tls_version` (just added) stays; (b) can gather them later if more is wanted.
4. **What a certificate is, to cppnet's users.** (a) `tls::certificate`: its DER and the fields commonly checked
   (subject, issuer, serial, validity, SANs, SHA-256 fingerprint), read with Botan at the handshake, Botan-free in the
   header; (b) only the DER, parsed by the user's own X.509 library; (c) a class wrapping Botan's certificate (behind a
   pointer, its accessors in `tls.cpp`), so anything Botan reads is reachable. **Recommended: (a)**: plain data, usable
   in builds without TLS (as `tls::version`), and enough to authorize by name, SPIFFE ID or pin; the DER is there for
   anything else.
5. **The client's certificate.** (a) One chain and key in `client_options` (paths, as the server's), sent when the
   server would accept it; (b) several, the first acceptable sent (Go's `Certificates`); (c) a callback choosing one per
   request (Go's `GetClientCertificate`). **Recommended: (a)**: one identity per client covers the uses seen (an API
   that requires mTLS); a client per identity covers the rest.
6. **Where sessions live.** (a) Tickets, from cppnet's own session manager (rotating keys, keys that can be given, bound
   to the client authentication); (b) Botan's `Session_Manager_Stateless` (one key from the credentials manager, no
   rotation); (c) Botan's `Session_Manager_In_Memory` on the server (stateful, no keys: sessions in a bounded map, lost
   on restart and not shared by a fleet). **Recommended: (a)**: Go's model; no memory per session; (b) has no rotation,
   and (c) neither shares across processes nor bounds a key's exposure any better.
7. **Resumption on by default?** (a) On (Go, nginx, Caddy); (b) off unless asked for. **Recommended: (a)**: it costs a
   ticket per handshake and saves the certificate work on every resumption; `session_tickets = false` turns it off.
8. **Which versions resume.** (a) TLS 1.3 and 1.2 (Go); (b) TLS 1.3 only. **Recommended: (b)**: a TLS 1.3 resumption
   still does its key exchange (Botan only implements `psk_dhe_ke`), so a stolen ticket key doesn't decrypt recorded
   sessions; a TLS 1.2 resumption doesn't, so it does. TLS 1.2 clients are few, and those keep full handshakes. (Go
   differs here.)
9. **Ticket keys and their lifetime.** (a) Random per context, rotated every `session_ticket_lifetime` (24 hours by
   default) keeping the previous key, or given (`session_ticket_keys`, `set_session_ticket_keys()`, which stops the
   rotation); (b) the same without given keys (no sharing across processes or restarts); (c) Go's numbers: tickets for 7
   days, a new key daily, kept 7 days. **Recommended: (a)**: a fleet behind a load balancer needs shared keys, and a
   day's lifetime (Botan's default, RFC 5246's suggestion for sessions) bounds a key's use to two days rather than Go's
   eight, at the cost of a full handshake for clients away more than a day.
10. **Resuming sessions with client certificates.** (a) Bind tickets to the client authentication (the key derived with
    the mode and the client CAs), and refuse an expired leaf or a missing certificate where one is required; (b) Go's:
    verify the session's chain again against the current client CAs on each resumption; (c) nothing beyond Botan's
    checks (lifetime, SNI). **Recommended: (a)**: as safe as (b) against a changed configuration (a ticket from another
    one doesn't decrypt), without the verification resuming saves; (c) would let a ticket outlive the client's
    certificate, or cross to a context that trusts other CAs.
11. **Trimming unconditionally or as an option.** (a) Always, in the overlay port (which only cppnet's manifest uses);
    (b) behind a port feature (vcpkg's features add, so it would be a `full` feature undoing the trimming); (c) only
    stop building the command-line tool. **Recommended: (a)**: cppnet's port exists for cppnet; a project wanting the
    rest of Botan uses its own manifest, and vcpkg's port, which cppnet works with too (the policy offers the same
    either way). (b) can be added if a cppnet user wants Botan for more.
12. **What the trimmed build (and the policy) keeps.** (a) As above: Go's groups (X25519MLKEM768 and Botan's other two
    hybrids, X25519, P-256, P-384, P-521), ECDSA and RSA certificates, AES-GCM and ChaCha20-Poly1305, and the CPUs'
    versions; (b) everything Botan's default policy offers (adding X448, brainpool, FFDHE and TLS 1.2's DHE, and their
    modules); (c) narrower: no PQ hybrids, no P-521. **Recommended: (a)**: what Go and browsers offer, so nothing a
    server or client in practice needs is missing; the hybrids are what's coming (Chrome and Firefox send X25519MLKEM768
    by default), and P-521 costs one module.
13. **The client's session cache.** (a) As it is (on, Botan's in-memory cache of 1000 sessions per context), keyed by
    port as well; (b) that and an option to turn it off (or size it); (c) off by default, as Go's. **Recommended: (a)**:
    resumption is what a client wants, and nothing has asked for less; an option is easy to add when something does.

## Out of scope (stage 4)

- **OCSP.** Let's Encrypt ended its OCSP service on 2025-08-06 (it had stopped putting OCSP URLs in certificates in May
  2025), leaving CRLs; Chrome hasn't checked OCSP for years (CRLSets), Firefox has moved to CRLite, and Go checks no
  revocation at all. cppnet's policy already doesn't require revocation data. Botan still checks an OCSP response a
  server staples (the client asks, as Botan's `support_cert_status_message()` does by default), which costs nothing;
  stapling one from cppnet's server isn't planned.
- **HTTP/2 over ALPN**: done (see Stages).
- Go's verification hooks (`VerifyPeerCertificate`, `VerifyConnection`) and the unverifying client authentication modes
  (Decided question 1); post-handshake client authentication; raw public keys.
- Choosing a certificate per SNI (Go's `GetCertificate`), and replacing a server's certificate while it runs (e.g. after
  an ACME renewal): a context is fixed once made, so a server restarts with a new one. Worth a stage of its own if
  certificates get shorter-lived (the CA/Browser Forum's 47-day limit is coming).
- 0-RTT (early data): Botan doesn't implement it, and it's replayable.
- Session IDs (stateful TLS 1.2 resumption), and storing tickets' sessions anywhere.
