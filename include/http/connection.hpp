#pragma once

#include <span>
#include <utility>
#include <variant>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"
#include "tcp.hpp"

#ifdef NET_HAS_TLS
#    include "tls/tls.hpp"
#endif

namespace net::http::detail
{

// connection is an HTTP connection, the client's or the server's: TCP, or TLS over TCP.
class connection final
    : public io::reader
    , public io::writer
{
public:
    explicit connection(tcp_socket conn) noexcept
        : conn{std::move(conn)}
    {}

#ifdef NET_HAS_TLS
    explicit connection(tls::stream conn) noexcept
        : conn{std::move(conn)}
    {}
#endif

    coro::task<io::result> read(std::span<std::byte> data) override
    {
        return std::visit([&](auto& c) { return c.read(data); }, conn);
    }

    coro::task<io::result> write(std::span<const std::byte> data) override
    {
        return std::visit([&](auto& c) { return c.write(data); }, conn);
    }

    using io::reader::read;
    using io::writer::write;

    // shutdown ends the outgoing side of the connection (over TLS, after close_notify), while reads still work.
    coro::task<> shutdown();

    // close closes the connection (over TLS, after close_notify, if shutdown() didn't send it).
    coro::task<> close();

    // socket is the TCP connection (under TLS's, if it's TLS), e.g. for deadlines and timeouts.
    [[nodiscard]] tcp_socket& socket() noexcept;

    // is_tls is whether it's a TLS connection.
    [[nodiscard]] bool is_tls() const noexcept;

    [[nodiscard]] int native_handle() const noexcept override;

private:
#ifdef NET_HAS_TLS
    std::variant<tcp_socket, tls::stream> conn;
#else
    std::variant<tcp_socket> conn;
#endif
};

}
