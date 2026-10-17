#include "http/connection.hpp"

#include <system_error>
#include <variant>

#include "coro/task.hpp"
#include "tcp.hpp"

#ifdef NET_HAS_TLS
#    include "tls/tls.hpp"
#endif

namespace net::http::detail
{

coro::task<> connection::shutdown()
{
#ifdef NET_HAS_TLS
    if (auto* tls = std::get_if<tls::stream>(&conn))
    {
        (void)co_await tls->shutdown();
        co_return;
    }
#endif

    std::get_if<tcp_socket>(&conn)->shutdown_write();
    co_return;
}

coro::task<> connection::close()
{
#ifdef NET_HAS_TLS
    if (auto* tls = std::get_if<tls::stream>(&conn))
    {
        co_await tls->close();
        co_return;
    }
#endif

    // The kernel still sends anything already written, without lingering here.
    std::get_if<tcp_socket>(&conn)->close(false);
    co_return;
}

tcp_socket& connection::socket() noexcept
{
#ifdef NET_HAS_TLS
    if (auto* tls = std::get_if<tls::stream>(&conn)) return tls->socket();
#endif
    return *std::get_if<tcp_socket>(&conn);
}

bool connection::is_tls() const noexcept
{
#ifdef NET_HAS_TLS
    return std::holds_alternative<tls::stream>(conn);
#else
    return false;
#endif
}

int connection::native_handle() const noexcept
{
#ifdef NET_HAS_TLS
    if (const auto* tls = std::get_if<tls::stream>(&conn)) return tls->native_handle();
#endif
    return std::get_if<tcp_socket>(&conn)->native_handle();
}

}
