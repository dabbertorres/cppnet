#pragma once

#include <memory>
#include <string_view>
#include <system_error>
#include <vector>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"
#include "tcp.hpp"
#include "tls/certificate.hpp"
#include "tls/version.hpp"

#ifdef NET_HAS_TLS
#    include <variant>

#    include "tls/tls.hpp"
#endif

namespace net::http::detail
{

// connection is an HTTP connection, the client's or the server's: TCP, or (with TLS) TLS over TCP. Either is a reader
// and writer (see as_reader() and as_writer()); the functions below do what they do differently.
#ifdef NET_HAS_TLS
using connection = std::variant<tcp_socket, tls::stream>;
#else
using connection = tcp_socket;
#endif

// as_reader and as_writer are the connection, as the reader and writer it is. NOTE: with TLS, they refer to what the
// variant holds, so are only valid while the connection isn't moved or assigned to.
inline io::reader& as_reader(connection& conn) noexcept
{
#ifdef NET_HAS_TLS
    if (auto* tls = std::get_if<tls::stream>(&conn)) return *tls;
    return *std::get_if<tcp_socket>(&conn);
#else
    return conn;
#endif
}

inline io::writer& as_writer(connection& conn) noexcept
{
#ifdef NET_HAS_TLS
    if (auto* tls = std::get_if<tls::stream>(&conn)) return *tls;
    return *std::get_if<tcp_socket>(&conn);
#else
    return conn;
#endif
}

// socket_of is the TCP connection (under TLS's, if it's TLS), e.g. for deadlines and timeouts.
inline tcp_socket& socket_of(connection& conn) noexcept
{
#ifdef NET_HAS_TLS
    if (auto* tls = std::get_if<tls::stream>(&conn)) return tls->socket();
    return *std::get_if<tcp_socket>(&conn);
#else
    return conn;
#endif
}

inline io::handle native_handle(const connection& conn) noexcept
{
#ifdef NET_HAS_TLS
    if (const auto* tls = std::get_if<tls::stream>(&conn)) return tls->native_handle();
    return std::get_if<tcp_socket>(&conn)->native_handle();
#else
    return conn.native_handle();
#endif
}

// alpn is the protocol a TLS connection agreed on with ALPN (empty if none, or it's not TLS).
inline std::string_view alpn([[maybe_unused]] const connection& conn) noexcept
{
#ifdef NET_HAS_TLS
    if (const auto* tls = std::get_if<tls::stream>(&conn)) return tls->alpn();
#endif
    return {};
}

// is_tls is whether it's a TLS connection.
inline bool is_tls([[maybe_unused]] const connection& conn) noexcept
{
#ifdef NET_HAS_TLS
    return std::holds_alternative<tls::stream>(conn);
#else
    return false;
#endif
}

// tls_version is the TLS version the connection agreed on (none if it's not TLS).
inline tls::version tls_version([[maybe_unused]] const connection& conn) noexcept
{
#ifdef NET_HAS_TLS
    if (const auto* tls = std::get_if<tls::stream>(&conn)) return tls->version();
#endif
    return tls::version::none;
}

// peer_certificates is the chain of certificates the peer sent, if it's a TLS connection and the peer sent one (e.g. a
// client asked for its certificate): null otherwise.
inline std::shared_ptr<const std::vector<tls::certificate>> peer_certificates([[maybe_unused]] const connection& conn)
{
#ifdef NET_HAS_TLS
    if (const auto* tls = std::get_if<tls::stream>(&conn))
    {
        if (auto certs = tls->peer_certificates(); !certs->empty()) return certs;
    }
#endif
    return nullptr;
}

// verifies_peer is whether it's a client's TLS connection whose handshake verified the server's certificate (see
// tls::detail::verifies()): false in cleartext, and with insecure_skip_verification.
inline bool verifies_peer([[maybe_unused]] const connection& conn) noexcept
{
#ifdef NET_HAS_TLS
    if (const auto* tls = std::get_if<tls::stream>(&conn)) return tls::detail::verifies(*tls);
#endif
    return false;
}

// verify_for verifies the server's certificate for another host than the connection's own, as a new connection to host
// would (see tls::detail::verify_for()), failing with why; or with std::errc::invalid_argument if the connection
// doesn't verify its peer (see verifies_peer()).
inline std::error_code verify_for([[maybe_unused]] const connection& conn, [[maybe_unused]] std::string_view host)
{
#ifdef NET_HAS_TLS
    if (const auto* tls = std::get_if<tls::stream>(&conn)) return tls::detail::verify_for(*tls, host);
#endif
    return std::make_error_code(std::errc::invalid_argument);
}

// tls_failure is whether err is a TLS connection's failure: an alert (either side's), or the peer breaking the
// protocol, which says why the connection ended, rather than only that it did (e.g. the peer closing or resetting it).
inline bool tls_failure([[maybe_unused]] std::error_code err) noexcept
{
#ifdef NET_HAS_TLS
    return err.category() == tls::category() && err != io::status_condition::closed;
#else
    return false;
#endif
}

// wait_writable waits until the connection can take more (see socket::wait_writable(): e.g. what's written but not yet
// sent is under its TCP_NOTSENT_LOWAT): over TLS, the TCP socket's, as the stream's writes wait for it (see
// tls::stream::wait_writable()), so never alongside them, nor a read's replies.
inline coro::task<io::result> wait_writable(connection& conn)
{
#ifdef NET_HAS_TLS
    if (auto* tls = std::get_if<tls::stream>(&conn)) return tls->wait_writable();
#endif
    return socket_of(conn).wait_writable();
}

// shutdown ends the outgoing side of the connection (over TLS, after close_notify), while reads still work.
inline coro::task<std::error_code> shutdown(connection& conn)
{
#ifdef NET_HAS_TLS
    if (auto* tls = std::get_if<tls::stream>(&conn)) return tls->shutdown();
#endif

    socket_of(conn).shutdown_write();
    return coro::ready_task<std::error_code>();
}

// close closes the connection (over TLS, after close_notify, if shutdown() didn't send it).
inline coro::task<> close(connection& conn)
{
#ifdef NET_HAS_TLS
    if (auto* tls = std::get_if<tls::stream>(&conn)) return tls->close();
#endif

    // The kernel still sends anything already written, without lingering here.
    socket_of(conn).close(false);
    return coro::ready_task<void>();
}
}
