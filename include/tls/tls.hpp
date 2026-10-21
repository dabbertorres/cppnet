#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "tcp.hpp"
#include "tls/certificate.hpp"
#include "tls/version.hpp"

// TLS (with Botan, which stays private: see docs/plans/tls.md). Only built if NET_HAS_TLS is defined.
namespace net::tls
{

// client_options configures a client_context.
struct client_options
{
    // The certificate authorities to trust: the system's (by default), or those in this PEM file instead (e.g. a
    // private CA's).
    std::optional<std::filesystem::path> trusted_ca_file{};

    // Skip verifying the server's certificate (and its name): for testing against self-signed servers only. Named so
    // that it's never set by accident.
    bool insecure_skip_verification = false;

    // The ALPN protocols to offer (e.g. {"http/1.1"}), in order of preference. Empty offers none.
    std::vector<std::string> alpn{};

    // Require TLS 1.2 servers to support the Extended Master Secret extension (RFC 7627), without which TLS 1.2 is open
    // to the triple handshake attack, refusing those that don't (with handshake_failure). Off by default, as in
    // browsers, Go and OpenSSL, as some servers still lack it. TLS 1.3 doesn't need it.
    bool require_extended_master_secret = false;

    // The protocol versions to allow, from min_version to max_version (as Go's MinVersion and MaxVersion): both, by
    // default. The handshake offers the latest; a server that allows none of them fails it with protocol_version.
    version min_version = version::tls12;
    version max_version = version::tls13;

    // PEM files: a certificate (followed by any intermediate certificates) and its private key, for a server that asks
    // for one (mutual TLS: see server_options::client_auth). Both, or neither. It's sent only if the server would
    // accept it: its kind of key among those the server asks for, and (if the server names the certificate authorities
    // it trusts, as cppnet's does) issued by one of them; otherwise the client sends none, as one without does.
    std::optional<std::filesystem::path> certificate_chain{};
    std::optional<std::filesystem::path> private_key{};
};

class stream;
class client_context;
class server_context;
struct server_options;

namespace detail
{

    struct client_state;
    struct server_state;
    struct connection;

    // policy_limits narrows what a client offers, for tests: e.g. one key exchange group, so that a handshake per group
    // shows it's built (as Botan's build may leave a module out, which only a handshake that uses it finds). Each is by
    // its code point in IANA's TLS registries, and limits the policy's to those it lists (empty: no limit).
    struct policy_limits
    {
        // TLS Supported Groups (e.g. 29 for X25519, 0x11EC for X25519MLKEM768).
        std::vector<std::uint16_t> groups{};
        // TLS Cipher Suites (e.g. 0x1301 for TLS_AES_128_GCM_SHA256).
        std::vector<std::uint16_t> cipher_suites{};
        // The groups to send TLS 1.3 key shares for in the first ClientHello, instead of the policy's choice (its first
        // hybrid post-quantum group and its first elliptic curve one): e.g. X25519's alone, as Botan's default policy
        // sends, so that a server preferring a hybrid asks for its share with a HelloRetryRequest. Those not among the
        // groups offered are left out.
        std::vector<std::uint16_t> key_shares{};
    };

    // make_client_context is client_context::make(), with what the client offers narrowed to limits.
    std::expected<client_context, std::error_code> make_client_context(const client_options& options,
                                                                       const policy_limits&  limits);

    // handshake_details are what a server's handshake agreed on, for tests.
    struct handshake_details
    {
        // whether it resumed a session.
        bool resumed = false;
        // the key exchange group, by its code point in IANA's TLS Supported Groups (as policy_limits'): TLS 1.3's (0
        // over TLS 1.2).
        std::uint16_t group = 0;
        // whether the server sent a HelloRetryRequest, asking the client for a key share for the group it chose, as the
        // first ClientHello had none.
        bool hello_retry = false;
    };

    // server_hooks are hooks into a server context, for tests.
    struct server_hooks
    {
        // what time it is, instead of the system clock: for tickets' ages, their keys' rotation, and verifying clients'
        // certificates.
        std::function<std::chrono::system_clock::time_point()> clock{};

        // called (on the thread running the handshake) as each handshake is done, with what it agreed on.
        std::function<void(const handshake_details&)> handshake_done{};
    };

    // make_server_context is server_context::make(), with hooks.
    std::expected<server_context, std::error_code> make_server_context(const server_options& options,
                                                                       server_hooks          hooks);

    // idle_clean is whether s holds nothing that a read hasn't taken: no data decrypted and unread, nor part of a
    // record; and neither the peer's close_notify nor a failure. For a pool of idle connections (whose sockets it
    // doesn't see into), deciding whether one may be reused.
    [[nodiscard]] bool idle_clean(const stream& s) noexcept;

}

// client_context holds what a client's connections share: the trusted certificate authorities (loaded once), its
// certificate and key (if any), a cache of sessions (to resume them), and the policy. It's cheap to copy, as copies
// share it.
class client_context
{
public:
    // make loads the trusted certificate authorities, and the certificate and key, failing if a file can't be read, or
    // (with std::errc::invalid_argument) if min_version is after max_version, or either isn't a version (e.g. none), or
    // if there's a certificate without a key or a key without a certificate, or the key isn't the certificate's.
    static std::expected<client_context, std::error_code> make(const client_options& options = {});

private:
    friend class stream;
    friend std::expected<client_context, std::error_code>
    detail::make_client_context(const client_options& options, const detail::policy_limits& limits);

    explicit client_context(std::shared_ptr<detail::client_state> state) noexcept;

    std::shared_ptr<detail::client_state> state;
};

// client_auth is whether a server asks for clients' certificates (mutual TLS), and whether it requires one: as Go's
// ClientAuth, but only its modes that verify what's sent.
enum class client_auth : std::uint8_t
{
    // not asked for (Go's NoClientCert).
    none,
    // asked for, and one that's sent must verify (VerifyClientCertIfGiven); a client may send none.
    optional,
    // one that verifies is required (RequireAndVerifyClientCert).
    required,
};

// session_ticket_key is a key session tickets are encrypted with (see server_options::session_ticket_keys): 32 random
// bytes.
using session_ticket_key = std::array<std::byte, 32>;

// server_options configures a server_context.
struct server_options
{
    // PEM files: the server's certificate (followed by any intermediate certificates), and its private key.
    std::filesystem::path certificate_chain{};
    std::filesystem::path private_key{};

    // The ALPN protocols the server speaks, in order of preference: the first the client also offers is chosen. A
    // client that offers none of them is still served (without ALPN).
    std::vector<std::string> alpn = {"http/1.1"};

    // Require TLS 1.2 clients to support the Extended Master Secret extension (see client_options).
    bool require_extended_master_secret = false;

    // The protocol versions to allow (see client_options): the latest the client offers is agreed on. A client that
    // offers none of them fails the handshake with protocol_version.
    version min_version = version::tls12;
    version max_version = version::tls13;

    // Whether to ask clients for their certificates (mutual TLS): see client_auth. A certificate a client sends must
    // chain to one of client_ca_file's certificate authorities (whose names the server sends, so the client knows which
    // of its certificates to send), and be for a client (its extended key usage, if any, including clientAuth); one
    // that doesn't fails the handshake with bad_certificate (with optional too). A client that sends none, where one's
    // required, fails it with certificate_required (TLS 1.3) or handshake_failure (TLS 1.2). In TLS 1.3, the client's
    // handshake is done before the server has looked at its certificate: so the client's connect() succeeds, and its
    // first read fails with the server's alert (in TLS 1.2, its connect() does). The stream's peer_certificates() are
    // the client's chain, and an HTTP server's handlers see them in server_request::peer_certificates.
    tls::client_auth client_auth = tls::client_auth::none;

    // PEM: the certificate authorities clients' certificates must chain to. Needed unless client_auth is none (and
    // refused then): never the system's (as Go's, without ClientCAs), as client certificates are a private CA's, and
    // trusting every public CA for them would let anyone with a web server's certificate in.
    std::optional<std::filesystem::path> client_ca_file{};

    // Session resumption, with tickets (as Go's, on by default): once a TLS 1.3 handshake is done, the server sends the
    // client a ticket (its session, encrypted with the server's key), with which the client's next connection resumes
    // the session, skipping the certificates (sending, signing for and verifying them, both ways) though still
    // exchanging keys, so a key that leaks later doesn't decrypt connections recorded before. Every connection, a
    // resumed one too, gets a ticket. TLS 1.2 sessions aren't resumed (their resumption skips the key exchange, so a
    // leaked key would decrypt them). stream::resumed() says whether a handshake resumed.
    //
    // Tickets are bound to the client authentication (client_auth, and client_ca_file's certificate authorities): a
    // ticket made where clients were verified differently isn't resumed (the handshake is a full one instead), even
    // with the same keys; nor is one whose client certificate has expired since, or that has none where one's now
    // required.
    bool session_tickets = true;

    // How long after it's made a ticket may be resumed with: at most 7 days (TLS 1.3's limit). Random keys are replaced
    // this often (see session_ticket_keys).
    std::chrono::seconds session_ticket_lifetime = std::chrono::hours{24};

    // The keys tickets are encrypted with (e.g. shared by a fleet of servers, so a client resumes with any of them):
    // the first makes tickets, and each is tried on them (so a new key can go first, the old one kept after it until
    // its tickets have expired). Keep them secret: with one, a server could be impersonated to clients resuming its
    // sessions (though connections recorded stay secret, as TLS 1.3's key exchange protects them). Empty (the default):
    // a random key, made with the context, and replaced (when tickets are next made or resumed) once it's
    // session_ticket_lifetime old, the previous one kept for as long again, so its tickets can still be resumed.
    // Refused without session_tickets.
    std::vector<session_ticket_key> session_ticket_keys{};
};

// server_context holds what a server's connections share: its certificate and key, the certificate authorities it
// trusts for clients' certificates (if it asks for them), the keys of its session tickets, and the policy. It's cheap
// to copy, as copies share it.
class server_context
{
public:
    // make loads the certificate chain and private key (and client_ca_file), failing if one can't be read, or (with
    // std::errc::invalid_argument) if min_version is after max_version, or either isn't a version (e.g. none), or the
    // key isn't the certificate's, or there's client_auth without client_ca_file, or client_ca_file without it, or
    // session_ticket_lifetime isn't more than 0 and at most 7 days, or there are session_ticket_keys without
    // session_tickets.
    static std::expected<server_context, std::error_code> make(const server_options& options);

    // set_session_ticket_keys replaces the keys tickets are encrypted with (as server_options::session_ticket_keys),
    // from any thread, while connections go on: the first makes tickets from then, and only these are tried on them.
    // Random keys are no longer replaced. Empty stops tickets being made or resumed. Without session_tickets, it does
    // nothing. The keys given are zeroed once they've been used.
    void set_session_ticket_keys(std::vector<session_ticket_key> keys);

private:
    friend class stream;
    friend std::expected<server_context, std::error_code> detail::make_server_context(const server_options& options,
                                                                                      detail::server_hooks  hooks);

    explicit server_context(std::shared_ptr<detail::server_state> state) noexcept;

    std::shared_ptr<detail::server_state> state;
};

// accept_options are stream::accept()'s.
struct accept_options
{
    // Requesting stop (from another task or thread) ends the handshake, failing with std::errc::operation_canceled.
    std::stop_token stop{};

    // The ALPN protocols to offer instead of the context's, if set (e.g. a server's, which decides what it speaks).
    std::optional<std::vector<std::string>> alpn{};

    // Don't answer the client's close_notify as it's read, so the stream can still be written after it (e.g. a server's
    // response to a request its client sent before closing), until shutdown() or close() answer it: from the
    // handshake, as a TLS 1.3 client may send it with its first request. Only TLS 1.3 lets a side write after the
    // peer's close_notify (RFC 8446, section 6.1); TLS 1.2 requires the answer at once (RFC 5246, section 7.2.1), which
    // Botan sends whatever this says.
    bool hold_close_reply = false;
};

// stream is a TLS connection over a TCP socket, which it owns: it reads and writes the application's data.
//
// A read and a write may run at once (e.g. on different threads), as with a socket; and as with a socket, only one
// read. Writes may, too: each is sent whole, in the order they were called. The socket's deadlines and timeouts (see
// socket()) limit them. A write returns once what it wrote has gone to the socket, after anything queued before it
// (e.g. another write, or a read's reply to the peer, which another task may be writing): so the socket's write
// deadline and timeout limit it, whichever task does the writing.
//
// Errors are the socket's, or TLS's (in category()): e.g. an alert from the peer, a certificate that couldn't be
// verified, or the connection ending without the peer's close_notify (stream_truncated). The peer's close_notify ends
// the stream (io::errc::end_of_stream), and is answered with close_notify, so nothing more can be written either
// (unless the answer's held: see accept_options). Both compare equal to io::status_condition::closed. A write that
// fails (e.g. times out) may have sent part of a record, so every write after it fails the same way.
class stream final
    : public io::reader
    , public io::writer
{
public:
    // connect runs a client's handshake over conn, with host (the server's name, for SNI, and which its certificate
    // must have), until deadline. Requesting stop (from another task or thread) ends it, failing with
    // std::errc::operation_canceled.
    static coro::task<std::expected<stream, std::error_code>> connect(tcp_socket            conn,
                                                                      client_context        ctx,
                                                                      std::string           host,
                                                                      io::clock::time_point deadline,
                                                                      std::stop_token       stop = {});

    // accept runs a server's handshake over conn, until deadline, or options.stop is requested (as connect).
    static coro::task<std::expected<stream, std::error_code>>
    accept(tcp_socket conn, server_context ctx, io::clock::time_point deadline, accept_options options = {});

    stream(const stream&)            = delete;
    stream& operator=(const stream&) = delete;

    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;

    // Closes the socket, without close_notify: close() sends it.
    ~stream() override;

    coro::task<io::result> read(std::span<std::byte> data) override;
    coro::task<io::result> write(std::span<const std::byte> data) override;

    using io::reader::read;
    using io::writer::write;

    // wait_writable waits until the socket is writable (see socket::wait_writable(): e.g. what's written but not yet
    // sent is under its TCP_NOTSENT_LOWAT), as writes wait for it: after a write (or a read's replies) being written,
    // never alongside it, which the socket would refuse. What's queued meanwhile (e.g. a read's replies, which leave
    // what they have to a write in progress) is written once it's writable, before it returns. Its result's count is 0,
    // and its err why it ended otherwise: the socket's write deadline, timeout and stop token apply, and a write that
    // failed before fails it at once.
    coro::task<io::result> wait_writable();

    // shutdown sends close_notify (telling the peer that nothing was cut short), and ends the socket's outgoing side,
    // without closing it: what the peer sent before its own close_notify can still be read, but nothing more written.
    coro::task<std::error_code> shutdown();

    // close sends close_notify, if shutdown() hasn't, and closes the socket.
    coro::task<> close();

    // alpn is the protocol the handshake agreed on: empty if none.
    [[nodiscard]] std::string_view alpn() const noexcept;

    // version is the protocol version the handshake agreed on.
    [[nodiscard]] tls::version version() const noexcept;

    // resumed is whether the handshake resumed an earlier session (see server_options::session_tickets), as Go's
    // DidResume. A resumption sends no certificates: peer_certificates() are those the session began with.
    [[nodiscard]] bool resumed() const noexcept;

    // peer_certificates is the chain of certificates the peer sent, leaf first: a client's is its server's, and a
    // server's its client's, if it asked for one and the client sent one (see server_options::client_auth); empty if
    // none. Each was verified (unless the client's context skips verification), and is what the peer sent, which may
    // not be the chain verification built (e.g. with the trusted root). Made the first time it's asked for, then
    // shared, so copies are cheap, and outlive the stream.
    [[nodiscard]] std::shared_ptr<const std::vector<certificate>> peer_certificates() const;

    // release_close_reply answers the peer's close_notify as the stream does by default (undoing accept_options'
    // hold_close_reply): if it was read already, with the next write (or shutdown()), and otherwise as it's read.
    void release_close_reply() noexcept;

    // set_stop_token makes reads and writes that wait for the socket end when stop is requested, failing with
    // std::errc::operation_canceled (see socket::set_stop_token(), which this calls). A canceled read leaves the
    // stream as it was, so it can be read again. A canceled write may have sent part of a record, so (as any write that
    // fails) it fails every write after it.
    void set_stop_token(std::stop_token stop) noexcept;

    // socket is the TCP socket the stream runs over: e.g. for its deadlines and timeouts. Don't read or write it.
    [[nodiscard]] tcp_socket& socket() noexcept;

    [[nodiscard]] io::handle native_handle() const noexcept override;

private:
    friend bool detail::idle_clean(const stream& s) noexcept;

    explicit stream(std::unique_ptr<detail::connection> conn) noexcept;

    // NOTE: on the heap, so that the socket (whose reads and writes the scheduler may be waiting on) doesn't move with
    //       the stream.
    std::unique_ptr<detail::connection> conn;
};

// Errors:
// - a fatal alert the peer sent (in category(), by its code in the TLS RFCs, e.g. 42 for bad_certificate);
// - one sent to the peer, for what was wrong here (in category(), as sent_alert_base plus its code);
// - the peer's certificate failing verification, for which bad_certificate was sent (in verification_category(), whose
//   message says why: e.g. it expired, or isn't for the host name);
// - or errc.
// Compare them with alert, which matches an alert whichever side sent it (and verification failures, bad_certificate);
// from_peer() says which side did.

// errc are TLS's errors, besides alerts.
enum class errc : std::uint16_t
{
    // the peer closed the connection without close_notify, so what was read may have been cut short.
    stream_truncated = 256,
    // a handshake, or an alert, that wasn't TLS (e.g. the peer isn't speaking TLS).
    protocol_error = 257,
};

// sent_alert_base is added to an alert's code for an error that sent it to the peer.
inline constexpr int sent_alert_base = 512;

// category is TLS's errors: alerts (the peer's, and those sent to it), and errc.
[[nodiscard]] const std::error_category& category() noexcept;

// verification_category is why the peer's certificate couldn't be verified.
[[nodiscard]] const std::error_category& verification_category() noexcept;

[[nodiscard]] std::error_code make_error_code(errc err) noexcept;

// alert are TLS's alerts (RFC 8446, section 6.2, and RFC 5246's), as conditions to compare errors with: e.g.
// err == tls::alert::handshake_failure.
enum class alert : std::uint8_t
{
    unexpected_message              = 10,
    bad_record_mac                  = 20,
    record_overflow                 = 22,
    handshake_failure               = 40,
    bad_certificate                 = 42,
    unsupported_certificate         = 43,
    certificate_revoked             = 44,
    certificate_expired             = 45,
    certificate_unknown             = 46,
    illegal_parameter               = 47,
    unknown_ca                      = 48,
    access_denied                   = 49,
    decode_error                    = 50,
    decrypt_error                   = 51,
    protocol_version                = 70,
    insufficient_security           = 71,
    internal_error                  = 80,
    inappropriate_fallback          = 86,
    user_canceled                   = 90,
    missing_extension               = 109,
    unsupported_extension           = 110,
    unrecognized_name               = 112,
    bad_certificate_status_response = 113,
    unknown_psk_identity            = 115,
    certificate_required            = 116,
    no_application_protocol         = 120,
};

[[nodiscard]] const std::error_category& alert_category() noexcept;

[[nodiscard]] std::error_condition make_error_condition(alert a) noexcept;

// from_peer is whether err is an alert the peer sent, rather than one sent to it.
[[nodiscard]] bool from_peer(std::error_code err) noexcept;
}

template<>
struct std::is_error_code_enum<net::tls::errc> : std::true_type
{};

template<>
struct std::is_error_condition_enum<net::tls::alert> : std::true_type
{};
