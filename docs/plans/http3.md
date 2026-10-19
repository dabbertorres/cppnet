# Plan: HTTP/3 (and QUIC)

Status: parked (2026-10-17), for after HTTP/2 ([http2.md](http2.md)), whose stream handling, `response_writer` back
ends and Huffman code it would reuse. Nothing is implemented, and its open questions are unanswered.

Until then, the server needs nothing to turn HTTP/3 away: clients only try HTTP/3 (over QUIC, on UDP) once a server
has advertised it, with Alt-Svc or an HTTPS DNS record, which cppnet doesn't; a client that tries anyway finds no UDP
socket listening (its system reports the port unreachable) and uses TCP, as curl's `--http3` falls back; and over TCP,
a request line of `HTTP/3.0` gets 505 (HTTP Version Not Supported), as any major version but 1 does.

## Goal

HTTP/3 in `http::server` and `http::client`: HTTP over QUIC, over UDP. QUIC is most of the work: a reliable,
congestion-controlled transport with its own streams, flow control and connection ids, whose handshake is TLS 1.3's,
carried without TLS's records. Whether to hand-roll QUIC, as the rest of cppnet is, or use a library, is the first
decision (see Open questions): this plan lays out both, and stages hand-rolling so that each step is checked against
other implementations before the next.

## Specifications and references

Required for a minimal, interoperable HTTP/3:

- [RFC 9000](https://www.rfc-editor.org/rfc/rfc9000), QUIC: packets, frames, streams, flow control, connection ids,
  the handshake, Retry, stateless resets, migration. With [RFC 8999](https://www.rfc-editor.org/rfc/rfc8999), the
  version-independent invariants.
- [RFC 9001](https://www.rfc-editor.org/rfc/rfc9001), QUIC with TLS: the handshake in CRYPTO frames, packet and header
  protection, Initial secrets, key updates. Its Appendix A has test vectors for packet protection.
- [RFC 9002](https://www.rfc-editor.org/rfc/rfc9002), loss detection and congestion control: ACKs, RTT estimation,
  the probe timeout (PTO), NewReno. Its Appendix A is pseudocode close to code.
- [RFC 9114](https://www.rfc-editor.org/rfc/rfc9114), HTTP/3: frames, control and unidirectional streams, GOAWAY, and
  discovery with Alt-Svc.
- [RFC 9204](https://www.rfc-editor.org/rfc/rfc9204), QPACK. A dynamic table capacity of 0 makes it the static table
  and Huffman coding (HPACK's code) only.
- [RFC 7838](https://www.rfc-editor.org/rfc/rfc7838), Alt-Svc: how clients learn a server speaks HTTP/3.

Optional, or later: [RFC 9218](https://www.rfc-editor.org/rfc/rfc9218) (priorities),
[9220](https://www.rfc-editor.org/rfc/rfc9220) (WebSockets over HTTP/3), [9221](https://www.rfc-editor.org/rfc/rfc9221)
and [9297](https://www.rfc-editor.org/rfc/rfc9297) (datagrams, for MASQUE and WebTransport),
[9368](https://www.rfc-editor.org/rfc/rfc9368) and [9369](https://www.rfc-editor.org/rfc/rfc9369) (version
negotiation, QUIC v2), [9460](https://www.rfc-editor.org/rfc/rfc9460) (HTTPS DNS records, the other way of discovering
HTTP/3), [9438](https://www.rfc-editor.org/rfc/rfc9438) (CUBIC), [8899](https://www.rfc-editor.org/rfc/rfc8899) (path
MTU discovery; QUIC only needs 1200-byte packets), [9287](https://www.rfc-editor.org/rfc/rfc9287) (greasing). Guidance:
[RFC 9308](https://www.rfc-editor.org/rfc/rfc9308) (applicability) and [9312](https://www.rfc-editor.org/rfc/rfc9312)
(manageability).

Even a minimal implementation needs: the anti-amplification limit (a server sends at most 3x what it has received from
an address it hasn't validated) and client Initials padded to 1200 bytes; the idle timeout; receiving key updates
(peers may update, and AEAD usage limits force it eventually); TLS_AES_128_GCM_SHA256 with AES header protection;
path validation when a NAT rebinds the client's address; and ignoring unknown frame and stream types.

Implementations to learn from, and to test against: [ngtcp2](https://github.com/ngtcp2/ngtcp2) and
[nghttp3](https://github.com/ngtcp2/nghttp3) (C, without I/O: the application feeds packets in, pulls packets out,
and drives the timers; curl's HTTP/3), [quic-go](https://github.com/quic-go/quic-go) (readable, and its UDP handling
per platform is a good reference), [picoquic](https://github.com/private-octopus/picoquic), and
[quiche](https://github.com/cloudflare/quiche) (Rust, also without I/O).

Tools: the [QUIC interop runner](https://github.com/quic-interop/quic-interop-runner) (its tests mostly use HTTP/0.9,
`hq-interop`, so QUIC can be checked before HTTP/3 exists; [results](https://interop.seemann.io/)),
[h3spec](https://github.com/kazu-yamamoto/h3spec) (error cases), qlog
([drafts](https://datatracker.ietf.org/doc/draft-ietf-quic-qlog-main-schema/)) with
[qvis](https://qvis.quictools.info/), Wireshark (decrypting with an SSLKEYLOGFILE), and curl's `--http3` and
`--http3-only`.

## Where things stand

- **Botan has no QUIC support**: no QUIC API (a way to run TLS 1.3's handshake without its records, getting each
  encryption level's secrets) and no QUIC implementation. Its issue
  ([#1961](https://github.com/randombit/botan/issues/1961)) is open; the maintainer expects, at most, "TLS 1.3 with the
  necessary hooks and primitives needed to implement QUIC in an external library", which don't exist yet (through
  3.13.0, 2026-08). What it has: a key-log callback that reports the handshake and traffic secrets, and callbacks to
  add and read extensions (for QUIC's transport parameters). Its primitives cover packet protection: AES-128-GCM and
  ChaCha20-Poly1305, AES for header protection, ChaCha20 with a counter, and HKDF (QUIC's HKDF-Expand-Label is built on
  its public HKDF-Expand).
- **TLS libraries with a QUIC API:** OpenSSL 3.5 and later (`SSL_set_quic_tls_cbs`, 2025; and its own QUIC client and
  server), BoringSSL and its relatives (aws-lc, quictls, LibreSSL 3.9.2 and later: `SSL_QUIC_METHOD`), wolfSSL,
  GnuTLS 3.7 and later, and picotls.
- **QUIC libraries** that fit a coroutine runtime are those without I/O of their own: ngtcp2 with nghttp3 (C, MIT,
  very active, in vcpkg; crypto through OpenSSL 3.5+, wolfSSL, GnuTLS, BoringSSL or picotls, so a second TLS library
  besides Botan), and quiche (needs Rust). msquic owns its sockets and threads, and lsquic needs BoringSSL.
- **UDP** in cppnet is minimal: `udp_socket` connects or binds, and has `receive_from`/`send_to` (`recvfrom`/`sendto`,
  since the DNS resolver plan's stage 1), but no `recvmsg`/`sendmsg`, batching, ECN or packet info.
- **Discovery**: browsers don't use HTTP/3 with prior knowledge. They learn of it from an `Alt-Svc: h3=":443"` header
  (so from a later connection), or HTTPS DNS records (Safari more than Chrome). So an HTTP/3 server also serves
  HTTP/1.1 and HTTP/2 over TCP, and advertises.

## How QUIC works

- **Packets** travel in UDP datagrams (several per datagram, in the handshake). Long headers (with the version and
  both connection ids) are for the handshake: Initial, 0-RTT, Handshake and Retry; short headers (just the
  destination connection id) for the rest. A packet's payload is frames.
- **Encryption levels and packet number spaces.** Initial packets are protected with keys derived from the client's
  first destination connection id (and a fixed salt): anyone can read them, but they're integrity-checked. The TLS
  handshake, in CRYPTO frames, then yields Handshake and 1-RTT (application) keys. Each of Initial, Handshake and
  application data has its own packet numbers and ACKs. Every packet is AEAD-protected, and its header (the packet
  number, and some flags) masked with a sample of the ciphertext (header protection).
- **Reliability.** Packets are never retransmitted; frames are. Receivers ACK packet numbers (in ranges, with a
  delay); a sender declares a packet lost when later ones are acknowledged (by count or time), or a probe timeout
  passes, and sends its frames' data again in new packets. Congestion control (NewReno in RFC 9002) bounds the bytes in
  flight.
- **Streams** are numbered (their low bits say which side opened them, and whether they're bidirectional), and carry
  ordered bytes in STREAM frames at offsets. Flow control limits each stream and the connection (MAX_STREAM_DATA,
  MAX_DATA), and how many streams each side may open (MAX_STREAMS). RESET_STREAM and STOP_SENDING end a direction.
- **Connection ids** let a connection survive a change of the client's address (a NAT rebinding, or migration), and
  let one UDP socket serve many connections: a server routes each packet by its destination id. Ids are issued with
  NEW_CONNECTION_ID and retired; a stateless reset ends a connection whose state is lost.
- **The handshake.** Client Initial (TLS ClientHello, with the transport parameters extension, padded to 1200 bytes);
  server Initial (ServerHello) and Handshake (the rest of the server's flight); client Handshake (Finished); then
  1-RTT. The server may first answer with Retry, to validate the client's address. ALPN is required (`h3`).
- **Ending.** An idle timeout (agreed in the transport parameters), CONNECTION_CLOSE (with a code; TLS alerts become
  0x100 plus the alert), or a stateless reset.

### HTTP/3 on QUIC

Each request is a bidirectional stream: HEADERS, DATA, optionally trailing HEADERS, then the stream's end. Each side
also opens a control stream (SETTINGS first, then GOAWAY and such), and QPACK's encoder and decoder streams.
Multiplexing, flow control and ordering are QUIC's, so HTTP/3 itself is small. QPACK is HPACK made safe for streams
that arrive out of order: with a dynamic table capacity of 0, it's the static table (99 entries) and literals.

## Design

### Layers

- **`io`: datagrams.** `udp_socket` gets what QUIC needs: binding, `recv_from`/`send_to` with addresses (through
  `recvmsg`/`sendmsg`, for ECN and the local address with `IP_PKTINFO` on multi-homed servers), and "don't fragment"
  (`IP_MTU_DISCOVER` on Linux, `IP_DONTFRAG` on macOS). Later, on Linux: batching (`recvmmsg`/`sendmmsg`) and
  segmentation offload (GSO, GRO). macOS has neither, so it reads a datagram at a time, as quic-go does.
- **`quic`: the transport.** A `quic::connection` is a state machine without I/O, as ngtcp2's and quiche's are: it's
  fed datagrams, asked for datagrams to send, and told the time (it reports its next deadline: an ACK delay, a probe
  timeout, the idle timeout). That makes it testable on bytes (and fuzzable), and the I/O a thin layer. A
  `quic::endpoint` owns a UDP socket and its connections: one receiving coroutine per socket (a socket allows one
  reader, and every connection shares it) routes each datagram by its connection id, and a connection's task sends
  what it has and sleeps until its deadline. Streams are `io::reader`s and `io::writer`s, so the code above them is
  written as it is for TCP.
- **`http`: HTTP/3.** The control stream, the frames, QPACK (sharing HPACK's Huffman code), and the request streams,
  served by the same machinery as HTTP/2's streams (a handler task per request, `response_writer`'s back end), with
  QUIC providing the multiplexing and flow control that HTTP/2 does itself.

### The TLS handshake

QUIC needs TLS 1.3's handshake messages without records, and each level's secrets as they're derived. With Botan,
there are three ways (an open question):

1. **Patch Botan** (cppnet already builds it from an overlay port, `ports/botan/`): add a mode in which its TLS 1.3
   channel hands over handshake messages instead of records, and reports secrets per level, as the QUIC APIs of other
   libraries do. It's the clean way, and the kind of hooks Botan's maintainer has said Botan would more likely offer
   than a QUIC implementation; it means learning Botan's TLS 1.3 internals, and keeping the patch working across
   Botan's releases (or getting it upstream).
2. **Drive unmodified Botan, and unwrap its records**: run its TLS 1.3 handshake normally, decrypt the records it
   emits (with the secrets its key-log callback reports), and wrap what the peer sends in records it accepts. It needs
   no patch, but it's fragile (it depends on the order of Botan's callbacks, which isn't documented) and wasteful.
3. **A second TLS library for QUIC's handshake** (OpenSSL 3.5's `SSL_set_quic_tls_cbs`, or wolfSSL's QUIC API): the
   documented way, and others have tested it, but it's a second TLS stack (to build, with its own trust store and
   certificate loading), for HTTP/3 alone.

Whichever it is, packet protection is cppnet's own (on Botan's primitives): it's QUIC's, not TLS's.

### Server and client

- **Server.** `server_config` gets HTTP/3 settings: on or off, the UDP port (usually the TCP port's number), and its
  limits. The server then listens on UDP as well as TCP, serves the same handler over both, and adds `Alt-Svc` to its
  TCP responses. HTTP/3 needs TLS (`server_config::tls`).
- **Client.** At first, HTTP/3 only when asked (a request or client option: prior knowledge, as `curl --http3-only`
  does). Later: an Alt-Svc cache, and racing QUIC against TCP for a host that advertised it (curl waits 100 to 200
  ms for QUIC before falling back).
- **Limits** carry over from HTTP/2's where they apply (header list size, concurrent streams, resets), plus QUIC's
  own: the anti-amplification limit, connections per address, stream and connection flow-control windows, Initial
  rate limiting (and Retry, when under load).

### Testing

- Packet protection against RFC 9001 Appendix A's vectors; varints, frames and packets on bytes; QPACK against its
  RFC's examples; loss detection and congestion control with simulated loss and reordering (the connection being
  without I/O makes this a unit test).
- Interop, each stage against other implementations: the handshake against ngtcp2, quic-go and OpenSSL; then
  transfers (`hq-interop`); then HTTP/3 against curl (`--http3-only`) and public servers.
- The QUIC interop runner (a Docker image of cppnet's client and server, running its test cases: handshake,
  transfer, chacha20, keyupdate, retry, resumption, http3, ...): by hand at first, perhaps in CI later.
- h3spec for error cases; fuzz targets for packets, frames, and QPACK; SSLKEYLOGFILE and qlog output, for Wireshark
  and qvis.

## Open questions

1. **Hand-roll QUIC, or use a library?** Hand-rolling fits the project (it's the most instructive part of HTTP/3),
   and a minimal QUIC v1 (no 0-RTT, no migration, NewReno) with HTTP/3 and static QPACK is roughly 10 to 20 thousand
   lines (ngtcp2's library is about 49 thousand, quic-go's transport about 30 thousand, with much more than the
   minimum). ngtcp2 with nghttp3 is the library that fits (no I/O of its own, in vcpkg, MIT), at the cost of a second
   TLS library. A middle way: ngtcp2 for QUIC, with HTTP/3 and QPACK hand-rolled on it.
2. **If hand-rolled, which handshake?** Patch Botan (recommended, as above), unwrap its records, or a second TLS
   library for QUIC alone? A first stage could be a spike to see how much Botan's patch takes.
3. **Platforms.** Linux with batching and segmentation offload, and macOS a datagram at a time (recommended), or the
   same (simple) path on both until it's measured?
4. **The client's discovery.** Prior knowledge only (recommended to start), or an Alt-Svc cache and racing QUIC
   against TCP from the start?
5. **What "minimal" includes.** Proposed: QUIC v1, NewReno, no 0-RTT, no active migration (but path validation for
   NAT rebinding, which the RFC requires), Retry and resumption later, QPACK without a dynamic table. Is that the right
   first target?
6. **The interop runner in CI.** Worth running (it needs Docker, and amd64), or by hand?

## Stages

1. **Datagrams.** `udp_socket`'s binding, `recv_from`/`send_to` (`recvmsg`/`sendmsg`), don't-fragment and packet
   info, tested over loopback.
2. **QUIC's codecs and cryptography.** Varints, packets and frames (without I/O); Initial secrets, HKDF-Expand-Label,
   packet and header protection, checked against RFC 9001's vectors. A fuzz target for packets.
3. **The handshake.** The TLS route (from the spike), CRYPTO frames per level, transport parameters, the
   anti-amplification limit, the idle timeout. Done when a client handshake completes with ngtcp2's and quic-go's
   servers, and theirs with cppnet's server.
4. **The transport.** ACKs, loss detection and the probe timeout, NewReno, flow control, streams, connection ids
   (issuing, retiring, routing on the server), stateless reset, receiving key updates, path validation. Done when
   `hq-interop` transfers pass the interop runner's handshake, transfer, multiconnect, chacha20 and keyupdate tests.
5. **HTTP/3.** The control stream, frames, QPACK without a dynamic table, request streams on HTTP/2's machinery, the
   server's UDP listener and Alt-Svc, the client with prior knowledge, graceful shutdown (GOAWAY). curl, h3spec, and
   the runner's http3 test.
6. **Later:** Retry and address validation tokens; resumption and 0-RTT; migration; initiating key updates; QPACK's
   dynamic table; CUBIC, pacing; batching, GSO and GRO, ECN; QUIC v2; the client's Alt-Svc cache and racing; datagrams
   (RFC 9221, 9297), WebTransport and WebSockets over HTTP/3; HTTPS DNS records (with the DNS resolver's plan).

## Out of scope

Server push (as for HTTP/2), multipath QUIC and the other drafts (until they're RFCs), and QUIC for anything but
HTTP/3 (the transport's API is general, but nothing else uses it).
