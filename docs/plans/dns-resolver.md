# Plan: an async DNS resolver

Status: proposed (2026-10-15). Nothing below is implemented yet.

## Goal

One asynchronous way to resolve host names that fits the coroutine runtime: lookups are `co_await`ed, bounded by
deadlines, canceled by `scheduler::request_stop()`, and fail with `std::expected` rather than exceptions.

## Where things stand

- `dns_lookup()` (`dns.hpp`) is a synchronous generator over `getaddrinfo()`. Only the tests use it.
- `tcp_socket::connect()` calls `getaddrinfo()` on `scheduler::run_blocking`, so it's already async for its caller.
- `udp_socket`'s and `listener`'s constructors call `getaddrinfo()` inline, blocking whichever thread runs them.
- `dns/message.hpp` has RFC 1035's message types (mostly the RFC's text), with no encoding or decoding.
  `dns/resolver.hpp` is a constructor and nothing else.

So there are four separate uses of `getaddrinfo()`, and none of the native resolver works.

## Model: Go's `net` package

Go has two resolvers, and picks between them:

- Its own resolver sends DNS queries itself (a blocked lookup costs a goroutine, not a thread), to the nameservers in
  `/etc/resolv.conf`. It reads `/etc/hosts`, and `/etc/nsswitch.conf` for the order of files and DNS; honors
  `resolv.conf`'s `search`, `ndots`, `timeout`, `attempts`, `rotate` and `single-request`; queries A and AAAA in
  parallel; falls back to TCP when a reply is truncated; and merges concurrent lookups of the same name. It has no
  cache, and lets callers replace how it reaches nameservers (`Resolver.Dial`).
- The C library's `getaddrinfo()` (blocking an OS thread, at most 500 at once) is used instead where Go's own resolver
  wouldn't give the system's answers: on macOS (whose configuration isn't all in `resolv.conf`: per-interface resolvers
  for VPNs, mDNS), when `LOCALDOMAIN`, `RES_OPTIONS` or `HOSTALIASES` are set, when `resolv.conf` or `nsswitch.conf`
  asks for something it doesn't implement, and for `.local` names.
- `GODEBUG=netdns=go|cgo` (or the `netgo`/`netcgo` build tags) forces one or the other.
- Separately, `Dialer` races IPv6 and IPv4 connections, starting the second family after a delay (RFC 6555).

## Design

Two backends behind one API:

- **system**: `getaddrinfo()` on `run_blocking`. A lookup can't really be canceled: on cancellation or timeout, the
  caller stops waiting, and the thread finishes the call in the background.
- **native**: queries sent with `udp_socket` (and `tcp_socket` for truncated replies), with the scheduler's deadlines.

Chosen like Go: native by default on Linux; system on macOS, when the configuration has something the native resolver
doesn't support, and for `.local` names. An option forces either one.

Rough shape (names to be settled in stage 1):

```cpp
namespace net::dns
{

struct resolve_options
{
    enum class backend : std::uint8_t { automatic, system, native };

    backend                                  use = backend::automatic;
    std::optional<io::clock::time_point>     deadline;
    // which address families to ask for: both, IPv4 only, or IPv6 only.
};

// resolve looks up host's addresses. An IP address literal is returned without a lookup.
coro::task<std::expected<std::vector<ip_addr>, std::error_condition>>
resolve(io::scheduler& sched, std::string_view host, resolve_options options = {});

}
```

## Stages

1. **One API, over `getaddrinfo()`.** Add `resolve()` with the system backend only. Use it in `tcp_socket::connect()`,
   and give `udp_socket` the same treatment. Replace `dns_lookup()`. `listener` can keep its synchronous lookup (it
   binds a local address, usually numeric). IP literals skip the lookup (no thread). Errors: decide whether
   resolution failures get their own condition enum (not found, no data, server failure, ...), which `gai_category()`
   errors map to, so both backends report the same conditions.
   Tests: `localhost`, IP literals without a thread, an unknown name's error, a deadline, and cancellation.
2. **Message codec.** Finish `dns/message.hpp`: encode queries, and decode responses defensively (bounds, name
   compression with a limit on pointers so loops can't hang it). Only A, AAAA and CNAME need their data decoded;
   other records are skipped by length. Drop the unused record types. A libFuzzer target, like the other parsers:
   decoding never crashes, and encoding then decoding round-trips.
3. **Native resolver.** Parse `/etc/resolv.conf` and `/etc/hosts`. Send queries with random IDs, and accept only
   replies that match (ID, question, server address). Retry across nameservers within the deadline; fall back to TCP
   on truncation; ask for A and AAAA in parallel; follow CNAME chains; apply the search list with `ndots`. Merge
   concurrent lookups of the same name. A hook to replace how nameservers are reached, so tests run against a fake
   DNS server on a loopback `udp_socket`, without the network.
4. **Choosing the backend.** Go's rule above, plus the option (and perhaps an environment variable, like
   `GODEBUG=netdns`) to force one.
5. **Later.** Happy Eyeballs (RFC 8305) in `tcp_socket::connect()`, using both families' results. A cache only if it's
   ever needed (Go does without one).

## Out of scope

DNSSEC validation, DNS over TLS/HTTPS (the nameserver hook leaves room for them), EDNS0 (truncation falls back to
TCP), and record types other than A, AAAA and CNAME.

## Open questions

- Ports: keep resolving only hosts (ports numeric, as `tcp_socket::connect()` already takes them), or also service
  names like `getaddrinfo()` does?
- Where `resolve()` lives: `net::dns`, replacing `dns.hpp`'s `dns_lookup()`, or `net::` alongside the sockets.
- Whether `udp_socket` gets an async factory like `tcp_socket::connect()`, or keeps its constructor with a
  pre-resolved address.
