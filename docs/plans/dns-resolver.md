# Plan: an async DNS resolver

Status: proposed (2026-10-15), revised (2026-10-19) for what has landed since: stop tokens on the scheduler's waits,
sockets and `connect`, `run_blocking(func, stop)`, `call_at`, the HTTP client's pool, TLS, and `std::expected`
throughout. The questions at the end (Decided questions) were decided as recommended (2026-10-19). Stages 1 and 2 are
done (2026-10-19); "Where things stand" describes the code before them.

## Goal

One asynchronous way to resolve host names that fits the coroutine runtime: lookups are `co_await`ed, bounded by a
deadline, ended by a `std::stop_token` (and by `scheduler::request_stop()`), and fail with `std::expected<T,
std::error_code>`, whose errors compare against conditions that both back ends share. `tcp_socket::connect()`, and so
the HTTP client's pool, resolve through it, so a request's deadline and token cover resolving its host as they already
cover connecting and the TLS handshake.

## Where things stand

- `dns_lookup()` (`dns.hpp`) is a blocking call of `getaddrinfo()` (`AI_ADDRCONFIG`, both families), returning
  `std::expected<std::vector<ip_addr>, std::error_code>` (errors in `gai_category()`), and skipping consecutive
  duplicates. Its users are `test/dns.cpp` (which needs the network), `example/ip-resolv`, and the guide.
- `tcp_socket::connect(scheduler, host, port, options)` calls `getaddrinfo()` on `run_blocking` (with
  `AI_NUMERICHOST` first, then `AI_ADDRCONFIG` for a name), so an IP literal still costs a hop to a blocking thread and
  back. `run_blocking(func, stop)` doesn't start `func` if stop's already requested, but nothing interrupts it once it
  has: `connect_options::stop` requested meanwhile only drops the result. `connect_options::timeout` covers connecting
  "over all of the host's addresses", but not resolving; the first address gets all of it (a timeout leaves "no time
  left to try the rest").
- `run_blocking`'s pool has 4 threads, and `connect` is its only user: it's the DNS pool in all but name.
- The HTTP client's pool (`connection_pool::acquire`) computes its deadline from `connect_timeout` before connecting,
  and passes `tcp_socket::connect` what's left as its timeout, so resolving isn't bounded by `connect_timeout` (its
  comment says it bounds connecting). Connections are keyed by scheme, host name and port; a new one resolves the host
  again. `http2_hosts` makes a burst of requests to a host known to speak HTTP/2 wait for the connection being made,
  so the burst resolves once; HTTP/1.1 bursts resolve per connection. TLS gets the URL's host for SNI and verification
  (`tls::stream::connect(conn, ctx, host, ...)`), never anything resolution produced.
- `udp_socket`'s constructors resolve with `getaddrinfo()` inline and create a socket of the first address's family,
  but neither bind nor connect it (and throw on failure): nothing uses `udp_socket`. `listener::make()` and its
  constructors call `getaddrinfo()` inline, which is fine for the local, usually numeric, addresses they bind.
- `dns/message.hpp` has RFC 1035's types (mostly the RFC's text, a bit-field header, every RR type of 1987), with no
  encoding or decoding. `dns/resolver.hpp`, an empty class, was removed (`f86aa5d`), as this plan replaces it.
  (CLAUDE.md's "Other modules" still lists a resolver under `dns/`.)
- `ip_addr` has no conversion to or from `sockaddr` (each of `tcp.cpp`, `udp.cpp`, `listen.cpp` and `dns.cpp` works
  with `addrinfo` directly), no IPv6 zone (scope id), and no port: there's no endpoint type.
- The pieces a resolver needs that exist now: `scheduler::wait_on(word, expected, deadline, stop)` and `notify_all()`
  (waits with a deadline and a token, on an atomic word); sockets' deadlines, timeouts and `set_stop_token()`;
  `scheduler::spawn()` and `async_scope`; `util::linked_stop_source`; `call_at()`. `coro::notifier` takes neither
  deadlines nor tokens, so it doesn't fit callers that must leave early. There's no `when_all`.

So there are still four separate uses of `getaddrinfo()`, a resolution that can't be cut short, and no native resolver.

## Model: Go's `net` package

Go has two resolvers, and picks between them:

- Its own resolver sends DNS queries itself (a blocked lookup costs a goroutine, not a thread), to the nameservers in
  `/etc/resolv.conf` (127.0.0.1 and ::1 if it lists none). It reads `/etc/hosts`, and `/etc/nsswitch.conf` for the
  order of files and DNS; honors `resolv.conf`'s `search`, `ndots`, `timeout`, `attempts`, `rotate`, `single-request`,
  `single-request-reopen`, `use-vc` and `trust-ad` (`edns0` is accepted and ignored: it always sends EDNS(0), offering
  1232 bytes); re-reads `resolv.conf` (and `/etc/hosts`) at most every 5 seconds, if it changed; queries A and AAAA in
  parallel; falls back to TCP when a reply is truncated; sorts the addresses by RFC 6724; and merges concurrent lookups
  of the same name (a `singleflight` group: a caller that gives up leaves, and the lookup is canceled only if no one
  else waits for it). It has no cache, and lets callers replace how it reaches nameservers (`Resolver.Dial`).
- The C library's `getaddrinfo()` (blocking an OS thread, at most 500 at once) is used instead where Go's own resolver
  wouldn't give the system's answers: on macOS (whose configuration isn't all in `resolv.conf`: per-interface
  resolvers for VPNs, mDNS), when `LOCALDOMAIN`, `RES_OPTIONS` or `HOSTALIASES` are set, when `resolv.conf` has an
  option it doesn't know, when `nsswitch.conf` names a source it doesn't implement (e.g. systemd's `resolve` or
  `mymachines`, and `myhostname` for the machine's own name), and for `.local` names. A caller whose context ends stops
  waiting at once; the C call finishes on its thread, and its result is dropped.
- `GODEBUG=netdns=go|cgo` (or the `netgo`/`netcgo` build tags) forces one or the other.
- Errors are `*net.DNSError`, with `IsNotFound`, `IsTimeout` and `IsTemporary`, set alike by both resolvers.
- `Dialer.Timeout` includes name resolution. With several addresses, each attempt gets a share of what's left (the
  remaining time over the remaining addresses, at least 2 s). `Dialer` races IPv6 and IPv4, starting the second family
  after `FallbackDelay` (300 ms; RFC 6555).

Elsewhere: c-ares is a native resolver driven by the caller's event loop (it hands out its sockets), with EDNS(0) and,
since 1.23, a TTL cache on by default; Rust's hickory-resolver is native and async, caching by default; tokio's
`lookup_host` and reqwest's default resolver are `getaddrinfo()` on `spawn_blocking`, detached when the future is
dropped.

## Decided

Kept from the 2026-10-15 plan, and nothing since contradicts them:

- **Two back ends behind one API**: the system's `getaddrinfo()` and a hand-rolled native resolver (no c-ares: the
  core is hand-rolled on purpose). Go's split is the model.
- **No cache** (Go does without one); if one's ever wanted, it goes behind the same API.
- **Results are addresses** (`std::vector<ip_addr>`), not canonical names: nothing downstream (TLS above all) may use
  a CNAME's target as the host's name.
- **Failures are `std::expected<T, std::error_code>`**: codes reported precisely, compared against conditions (the
  2026-10-15 sketch returned an `std::error_condition`, which CLAUDE.md's convention rules out).

## Design

### The API

Names, settled in stage 2 (Decided questions 1 and 8):

```cpp
namespace net::dns
{

struct resolve_options
{
    protocol                             proto = protocol::not_care; // both families, or IPv4 or IPv6 only
    std::optional<io::clock::time_point> deadline;                   // none: the back end's own limits
    std::stop_token                      stop{};                     // fails with operation_canceled
};

class resolver
{
public:
    enum class backend : std::uint8_t { automatic, system, native };

    struct options
    {
        backend use = backend::automatic;
        // native: the configuration, instead of reading /etc/resolv.conf and /etc/hosts (tests, and programs that
        // want their own nameservers): nameservers as addresses and ports, search list, ndots, timeout, attempts, ...
        std::optional<config> conf;
    };

    explicit resolver(options opts = {});

    // resolve looks up host's addresses. An IP address literal is returned without a lookup (no thread, no query).
    coro::task<std::expected<std::vector<ip_addr>, std::error_code>>
    resolve(io::scheduler* scheduler, std::string host, resolve_options options = {});
};

// default_resolver is the process's (Go's net.DefaultResolver): what connect() uses unless given another.
resolver& default_resolver();

}
```

- `host` by value, as `connect()` takes it: a coroutine mustn't hold a view of its caller's string.
- `protocol` is the sockets' existing enum (`not_care`, `ipv4`, `ipv6`), not a new one.
- The resolver holds its state (configuration read and when, lookups in flight) behind a `shared_ptr`, so lookups
  that outlive a caller (below) outlive the resolver too. It isn't tied to a scheduler: each `resolve()` runs on the
  caller's.
- Names are checked before anything's sent (labels of 1 to 63 bytes, at most 253 in all, ASCII; Go's `isDomainName`
  allows underscores, as it must for real names): `std::errc::invalid_argument` otherwise. No IDNA.

### Errors

(Open question 5.) `dns::errc` (`dns::category()`) for what the native resolver finds: `name_not_found` (NXDOMAIN
after the search list), `no_address` (the name exists, without records of the family asked for), `server_failure`
(SERVFAIL, or nothing usable, from every server), `refused`, `bad_response` (malformed, or a reply that matches no
query). `dns::condition` (`not_found`, `temporary_failure`) to compare against, to which `dns::errc` and
`gai_category()`'s codes (`EAI_NONAME`, `EAI_NODATA` where defined, `EAI_AGAIN`, `EAI_FAIL`) are both equivalent: the
condition category's `equivalent()` knows `gai_category()`, so `exception.cpp` doesn't change. A deadline is
`std::errc::timed_out` and a stop `std::errc::operation_canceled`, as every other wait's, so `io::status_condition::
timed_out` matches them. Callers that compared `err.category() == gai_category()` (two tests) compare with
`dns::condition::not_found` instead.

### Deadlines, cancellation and lookups in flight

A lookup (anything but a literal) is shared state (the result, an atomic word, a count of waiters, its own
`std::stop_source`) and a task spawned on the caller's scheduler that does the work. Callers wait for it with
`scheduler::wait_on(word, 0, deadline, stop)`, so a deadline or a stop ends the caller's wait at once, whatever the
back end is doing. A caller that leaves takes itself off the count; the last one to leave stops the lookup's source
(outside the scheduler's locks, as stop must be). The lookup's task stores the result, sets the word and calls
`notify_all()`. `scheduler::request_stop()` cancels the waits (callers get `operation_canceled`) and the native
lookup's socket waits.

- **Merging** (stage 3): the resolver keeps lookups in flight in a map (under its own lock), keyed by scheduler (a
  wait on a word is a scheduler's), lower-cased name, family and back end. A second caller for the same key joins the
  lookup rather than starting one, as Go's `singleflight` does, and leaving cancels it only if no one else waits.
- **Why not `coro::notifier`**: it has no deadlines or tokens (by design: `handler-cancellation.md`), and every caller
  here must be able to leave on its own.
- **Why not `call_at`**: every deadline here is a wait's (`wait_on`'s, or a socket's read deadline), which the
  scheduler already times; `call_at` is for a deadline with no wait to hang it on. It may serve Happy Eyeballs' delays
  (stage 7).

### The system back end

`getaddrinfo()` on `run_blocking` (Decided question 4), as `connect` does now (`AI_ADDRCONFIG` for names, so a host
without IPv6 isn't handed AAAA records first), inside the lookup's task. It can't be interrupted, so cancellation and
deadlines detach the caller rather than end the call (Decided question 3): the thread finishes it, and the result is
dropped, as Go's cgo path and tokio's `lookup_host` do. Until then it holds one of `run_blocking`'s threads, so stuck
lookups (a nameserver that doesn't answer: glibc's defaults are 5 s and 2 attempts per server) can starve the rest,
and `scheduler::shutdown()` waits for them, as it waits for every task. Merging (stage 3) keeps a burst for one host
on one thread.

### The native back end

- **Configuration**: `/etc/resolv.conf` (`nameserver`, `search`/`domain`, `options ndots timeout attempts rotate
  single-request single-request-reopen use-vc edns0 trust-ad`; anything else marks it unsupported, for the automatic
  choice), `/etc/hosts`, and `nsswitch.conf`'s `hosts:` line, each parsed by a pure function of the file's text, so
  they're tested on strings. Re-read at most every 5 s, if `stat()` says they changed (Go's rule). Nameservers with an
  IPv6 zone (`fe80::1%eth0`) need `ip_addr` to carry one: skipped, until it does.
- **Queries**: per nameserver attempt, a `udp_socket` connected to the server's address (a new one each time, so the
  kernel picks a fresh random source port; a connected socket also drops datagrams from anyone else, and reports an
  ICMP refusal as `connection_refused`, which moves on to the next server at once). The A and AAAA queries go out on
  the same socket, each with its own ID, and one coroutine reads until both are answered: parallel without a second
  task (glibc does the same); `single-request` sends them in turn, `single-request-reopen` on a new socket. The read
  deadline is the attempt's (`timeout`) or the caller's, whichever is first, and the socket gets the lookup's token
  (`set_stop_token()`). A reply counts only if its ID, question and opcode match a query still open; anything else is
  read past. (An empty datagram reads as `io::errc::end_of_stream` from `socket::read`: read past it too.)
- **IDs** from the OS's randomness (`arc4random_buf` on macOS and BSD, `getrandom()` on Linux), buffered, not a seeded
  `std::mt19937` (predictable after a few outputs), and not Botan (the build without TLS has none).
- **EDNS(0)** (Decided question 7): an OPT record offering 1232 bytes, with no options and no DO bit, as Go sends; a
  FORMERR answer to it retries without.
- **Truncation**: TC set, the same query over TCP to the same server (`tcp_socket::connect` to the address, stage 1's
  overload, so no resolution and no thread), framed by its two-byte length, through a `buffered_reader`.
- **Retries**: `attempts` rounds over the servers (`rotate` starts each lookup at the next one), within the caller's
  deadline; SERVFAIL and REFUSED move to the next server; NXDOMAIN ends that name (the search list goes on).
- **Names**: the search list with `ndots` (a name with at least `ndots` dots is tried as is first; one ending in a dot
  only as is), `/etc/hosts` before or after DNS as `nsswitch.conf` orders them, and CNAME chains followed within the
  answer (at most 8 links, a loop being `bad_response`).
- **Order**: RFC 6724's destination address selection (Go's `addrselect.go`), using the source address the kernel
  would choose (a UDP socket connected to each address, then `getsockname()`: syscalls, no packets). `getaddrinfo()`
  already sorts its results, so both back ends give the same order for the same answers.

### Choosing the back end

`automatic` follows Go (Decided question 6): native on Linux and the BSDs, unless the configuration has something the
native resolver doesn't do (an unknown `resolv.conf` option, `LOCALDOMAIN`/`RES_OPTIONS`/`HOSTALIASES` set, an
`nsswitch.conf` source other than `files` and `dns`, or one with non-default actions); system on macOS, for `.local`
names, and for the machine's own name when `nsswitch.conf` has `myhostname`. `NET_DNS=system|native` (cppnet's
`GODEBUG=netdns`) and `resolver::options::use` force one. Note: Arch's default `nsswitch.conf` lists `mymachines` and
`resolve` before `dns`, so `automatic` chooses the system there (CI's Linux container, unless its image differs); on
Ubuntu's server images (`files dns`, `resolv.conf` pointing at systemd-resolved's stub) it chooses the native. (To
check on the CI image before stage 6, as its tests don't depend on it.)

### Connecting

- `tcp_socket::connect(scheduler, std::span<const ip_addr> addresses, std::uint16_t port, options)` (stage 1):
  connects to given addresses, in order, without resolving. Each attempt gets a share of what's left of the timeout
  (Go's rule: the remaining time over the remaining addresses, at least 2 s, never past the end), rather than the first
  taking all of it.
- `tcp_socket::connect(scheduler, host, port, options)` resolves through `connect_options::resolver` (a pointer; null
  is `default_resolver()`), with `options.stop` and the connection's deadline, then connects to the addresses. Its
  timeout covers both (Decided question 2), so the stop and the deadline reach resolving; the comment about
  `getaddrinfo()`'s own timeouts goes.
- `udp_socket` (Decided question 9): `udp_socket::connect(scheduler, ip_addr, port)`, returning `std::expected`, for the
  resolver's queries; `udp_socket::bind(scheduler, ip_addr, port)` with `receive_from()`/`send_to()` for servers (the
  tests' fake nameserver); the resolving constructors, which make an unbound, unconnected socket, go.
- `listener` keeps its synchronous `getaddrinfo()`: it binds local addresses.
- **The HTTP client** gets `client_options::resolver` (passed to `connect`), and its pool's `connect_timeout` then
  bounds resolving too, as its comment already reads. Nothing about keys changes: connections stay keyed by host name
  (as Go's `Transport` keys them), and TLS keeps the URL's host for SNI and verification; IP literals stay literals.
  `http2_hosts` and merging together mean a burst of requests to one host resolves once.

## Costs

- **IP literals**: no thread and no lookup, where `connect` now hops to a blocking thread and back for them.
- **System**: per lookup, the shared state, `spawn()`'s allocations and the task's frame, on top of today's
  `run_blocking` frame and hop: a few allocations per new connection, against a `getaddrinfo()` call. A caller that
  gives up leaves a thread busy until the call returns, and `scheduler::shutdown()` waits for it.
- **Native**: per lookup, the same shared state and task, a UDP socket per attempt (an fd, registered with the
  scheduler), a buffer of 1232 bytes (64 KiB for TCP), and the RFC 6724 sort's sockets (syscalls only). Configuration:
  a `stat()` per file at most every 5 s.
- **Nothing per request** on a connection the pool already has: resolution is per new connection.

## Testing

No new test needs the network: the existing `test/dns.cpp` lookups of public names stay the only ones that do.

- **Stage 1**: connecting to IP literals while `run_blocking`'s threads are all busy (four functions blocked on a
  latch) succeeds, so no thread was used; the address-list overload tries the next address after a refusal (a closed
  port, then a listener), and splits the timeout (a non-routable address first, then a listener, within the timeout);
  `udp_socket` connected and bound, an exchange on 127.0.0.1 and ::1.
- **Stage 2**: the system back end through an injected lookup function (a test hook in `resolver::options`, internal)
  that blocks until released: a deadline returns `timed_out` at the deadline while the call is still blocked, a stop
  returns `operation_canceled`, `request_stop()` likewise, and the call's late result is dropped without a crash
  (ASan); an injected `EAI_NONAME` compares equal to `dns::condition::not_found`. With the real `getaddrinfo()`:
  `localhost` (loopback addresses only, the existing byte-order check), literals of both families without a thread.
  `connect` and the client: a stopped token ends a connect that's resolving; `connect_timeout` bounds a resolution
  that hangs (the hook).
- **Stage 3**: a counting hook: concurrent lookups of one name start one call; one caller leaving doesn't end the
  others'; the last leaving stops it (the native back end's queries, once it exists: the fake server sees no retry).
- **Stage 4**: the codec on hand-made messages (RFC 1035's examples, compressed names, OPT records) and captured
  replies; rejects: pointer loops and forward pointers, labels over 63, names over 255, counts past the data, RDLENGTH
  past the message. `fuzz_dns` (libFuzzer, as `fuzz/`'s others, a 10 s `fuzz`-labelled ctest run): decoding never
  crashes, and a decoded message re-encodes to bytes that decode the same.
- **Stage 5**: parsers of `resolv.conf`, `hosts` and `nsswitch.conf` on strings. A fake nameserver
  (`test/dns/fake_server.hpp`): a bound `udp_socket` on 127.0.0.1 (and ::1) port 0, and a `listener` on the same port
  for TCP, answering from a script (delays, drops, wrong ID, wrong question, truncation, SERVFAIL, REFUSED, NXDOMAIN,
  CNAME chains and loops, EDNS FORMERR), with the resolver given its address and port in `options::conf`. Retries and
  rotation across two fake servers, search and `ndots`, `hosts` before DNS, both families on one socket and with
  `single-request`, a stop or deadline during a wait, `request_stop()`. These run on macOS too (the configuration is
  injected, whatever `automatic` would choose), and on Linux under TSan (merging, leaving and completing race:
  loops, as the scheduler's tests have).
- **Stage 6**: the choice from configurations given as strings and environment variables (Arch's and Ubuntu's
  `nsswitch.conf`, unknown options, `.local`).
- CI as it is: the Linux and macOS jobs run all of it; TSan covers stages 3 and 5's races; the build without TLS
  builds the resolver (nothing in it needs Botan).

## Stages

Each is mergeable alone, with its tests and docs (`docs/guide.md`, CLAUDE.md, README where it says so).

1. **Connecting to addresses.** `ip_addr` to and from `sockaddr` (internal), the address-list `connect` overload with
   Go's per-address share of the timeout, `connect(host, ...)` taking literals without a thread, and `udp_socket`'s
   `connect`, `bind`, `receive_from` and `send_to` (Decided question 9). Useful alone; stages 2 and 5 build on it.
   **Done** as above: `detail::to_sockaddr()`/`from_sockaddr()` (`sockaddr.hpp`), `connect(scheduler, addresses,
   port, options)` (a plain function, copying the addresses before it returns; addresses not of `options.proto`'s
   family are skipped), `detail::attempt_deadline()` for the share (a timed-out attempt moves on to the next), and
   `udp_socket::connect`/`bind` (synchronous, returning `std::expected`) with `receive_from()`/`send_to()` (coroutines,
   through the socket's own read and write operations, so its deadlines, timeouts and token apply). A literal takes
   the fast path only in its usual form (an IPv4 one as `to_string()` writes it, as `getaddrinfo()` reads e.g.
   `010.0.0.1` as octal) with a numeric port; the rest (service names as ports, IPv6 zones) still goes to
   `getaddrinfo()` until stage 2, whose addresses keep their zones.
2. **`dns::resolver` over `getaddrinfo()`.** The API, `default_resolver()`, the errors and conditions, the lookup's
   shared state and task with callers waiting on `wait_on` (deadline and stop), `connect_options::resolver` and
   `client_options::resolver`, `connect`'s timeout covering resolving (Decided question 2). `dns_lookup()` and `dns.hpp`
   go (the example uses `run_to_completion`); the guide's "resolving can't be interrupted" lines, CLAUDE.md's `connect`
   and module notes, and `test/dns.cpp`/`test/tcp.cpp`'s category checks change.
   **Done** as above. `backend` has only `automatic` and `system`, and `options` no `conf`, until stage 5; the test hook
   is `options::system_lookup`. `resolve()` isn't a coroutine: a stopped token, a literal (its usual form, as in stage
   1) and an invalid name are ready tasks; a lookup's task is spawned when the caller's is awaited, and runs
   `run_blocking(func, stop)` with the lookup's token, so one whose caller left before it had a thread doesn't start.
   Other numeric forms (`127.1`) are valid names, read by `getaddrinfo()` as before (so Go's "a name has a non-digit"
   isn't kept); a zone isn't (`invalid_argument`), so `connect` reads a host with one inline with
   `getaddrinfo(AI_NUMERICHOST)`, keeping the zone. `temporary_failure` matches `EAI_FAIL` too; `refused` and
   `bad_response` match neither condition. `io::scheduler::options::blocking_threads` (4) sizes `run_blocking`'s pool.
3. **Merging lookups in flight**, Go's `singleflight` with `ForgetUnshared`.
4. **The message codec.** `dns/message.hpp` rewritten: a header encoded field by field (no bit-field layout), queries
   (with the OPT record), and responses decoded defensively (bounds; compression pointers only backwards, at most a
   fixed number per name, so loops can't hang it); only A, AAAA and CNAME data decoded, other records skipped by
   length; the other 1987 record types dropped. `fuzz_dns`. Independent of 1 to 3; can land any time.
5. **The native back end**, opt-in (`backend::native`): configuration and its parsers, queries over UDP and TCP, IDs,
   EDNS(0), retries, search, `hosts`, CNAMEs, RFC 6724 order. The fake nameserver.
6. **`automatic`**: Go's rule, and `NET_DNS`. Until this lands, `automatic` is the system back end.
7. **Later.** Happy Eyeballs (RFC 8305) in `connect`: attempts staggered by 250 ms (`call_at`, or a `sleep_until` with
   the token), losers stopped through a `util::linked_stop_source` linked to `options.stop`; with the native back end,
   connecting on the first family's answer (RFC 8305's resolution delay). A cache, only if ever needed. A transport
   hook (Go's `Resolver.Dial`) when DNS over TLS or HTTPS is wanted.

## Decided questions

All decided as recommended (2026-10-19); the options are kept for the reasoning.


1. **The API's shape and home.** (a) `dns::resolver` objects, a process-wide `default_resolver()`, and
   `connect_options::resolver`/`client_options::resolver` pointers, in `net::dns` (`dns/resolver.hpp`), replacing
   `dns.hpp`; (b) a free `resolve()` only, its state hidden in a global; (c) a resolver owned by the scheduler (but `io`
   would then depend on `dns`, which depends on `io`'s sockets). Go: `net.DefaultResolver` and `Resolver` values,
   given to `Dialer.Resolver`; c-ares: a channel per configuration; hickory: a `Resolver` value, no global.
   **Recommended: (a)**, with `dns_lookup()` removed rather than kept as a blocking wrapper.
2. **Does `connect`'s timeout cover resolving?** (a) Yes: one deadline for resolving and connecting, as Go's
   `Dialer.Timeout`; the pool's `connect_timeout` then means what its comment says. (b) No, as now (resolving bounded
   only by the back end's own limits). (c) A separate resolve timeout. **Recommended: (a).** Changes behavior for
   callers whose resolution is slow: their connects time out where they didn't.
3. **Canceling a system lookup.** (a) Not at all, as now: the caller waits for `getaddrinfo()`. (b) The caller
   leaves at its deadline or stop; the call finishes on its thread and its result is dropped (Go's cgo path, tokio's
   `lookup_host`, reqwest's `GaiResolver`). (c) The platforms' async APIs: macOS's `DNSServiceGetAddrInfo` (its socket
   polled by the kqueue loop, `DNSServiceRefDeallocate` to cancel), glibc's `getaddrinfo_a` (whose `gai_cancel` only
   cancels requests not yet started): two more back ends, unlike each other. **Recommended: (b).**
4. **Where system lookups run.** (a) `run_blocking`'s pool, as now (4 threads, used only by `connect`), with its size
   made a scheduler option; `scheduler::shutdown()` waits for lookups in flight, as tokio's runtime waits for its
   blocking tasks when dropped. (b) The resolver's own threads, outside the scheduler's tasks, so shutdown doesn't wait
   for them (Go: up to 500 threads in C lookups; exiting doesn't wait). **Recommended: (a)**, with merging; (b) if a
   stuck lookup delaying shutdown is ever a problem.
5. **Errors.** (a) `dns::errc` for the native back end and `dns::condition` (`not_found`, `temporary_failure`) that
   both it and `gai_category()` are equivalent to; (b) the native back end reports `gai_category()` codes
   (`EAI_NONAME`, ...), one category for both; (c) `std::errc` only (`no_such_device_or_address`?), losing what
   happened. Go: `DNSError`'s `IsNotFound`, `IsTimeout`, `IsTemporary`, set by both resolvers; c-ares: `ARES_ENOTFOUND`,
   `ARES_ENODATA`, `ARES_ESERVFAIL`, `ARES_ETIMEOUT`, ...; hickory: `NoRecordsFound` with the response code, `Timeout`.
   **Recommended: (a)**, the repo's codes-and-conditions convention.
6. **What `automatic` chooses.** (a) Go's rule (native on Linux unless the configuration needs the system; system on
   macOS and for `.local`); (b) the system everywhere, native only when asked for; (c) native everywhere but macOS,
   ignoring `nsswitch.conf`. **Recommended: (a)**, reached in stage 6 (the system until then). Under (a), Arch (and so
   CI's Linux container) gets the system back end because of its `nsswitch.conf`, which the tests don't depend on.
7. **EDNS(0).** (a) Out of scope, as the 2026-10-15 plan had it: 512-byte UDP replies, TCP when truncated. (b) A
   minimal OPT record offering 1232 bytes (no options, no DNSSEC), retrying without on FORMERR. Go always sends it
   (and ignores `resolv.conf`'s `edns0` as already on); c-ares and hickory send it by default. **Recommended: (b)**: a
   few lines in the codec, and fewer TCP fallbacks for names with many addresses.
8. **Ports and service names.** (a) Numeric ports only: the resolver takes none, and `connect` fails with
   `invalid_argument` for a name like `"http"` (which `getaddrinfo()` accepts today); (b) a small built-in table (Go
   has one for a few names, and reads `/etc/services`); (c) names through the system back end. The HTTP client passes
   numeric ports already, and nothing in the repo passes a name. **Recommended: (a).** Taking `std::uint16_t` rather
   than a string is a further, breaking, option.
9. **`udp_socket`.** (a) `connect(scheduler, ip_addr, port)` for clients, `bind()` with `receive_from()`/`send_to()`
   for servers, replacing the resolving constructors (Go: `DialUDP`, and `ListenUDP` with `ReadFrom`/`WriteTo`); (b)
   only `connect()`, the tests' fake server using raw syscalls in a helper; (c) leave `udp_socket` alone, the resolver
   using its own descriptors. **Recommended: (a)**: the README promises UDP sockets, and the fake server is a real user
   of `bind()`.

## Out of scope

DNSSEC validation, DNS over TLS/HTTPS (stage 7's transport hook leaves room), record types other than A, AAAA, CNAME
and OPT, reverse lookups (PTR), mDNS (`.local` goes to the system back end), IPv6 zones (until `ip_addr` has them), and
IDNA.
