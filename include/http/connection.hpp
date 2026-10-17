#pragma once

#include <system_error>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"
#include "tcp.hpp"

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

// is_tls is whether it's a TLS connection.
inline bool is_tls([[maybe_unused]] const connection& conn) noexcept
{
#ifdef NET_HAS_TLS
    return std::holds_alternative<tls::stream>(conn);
#else
    return false;
#endif
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
