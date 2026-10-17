#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
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

// TLS (with Botan, which stays private: see docs/plans/tls.md). Only built if NET_HAS_TLS is defined.
namespace net::tls
{

using namespace std::chrono_literals;

// client_options configures a client_context.
struct client_options
{
    // The certificate authorities to trust: the system's (by default), or those in this PEM file instead (e.g. a private
    // CA's).
    std::optional<std::filesystem::path> trusted_ca_file{};

    // Skip verifying the server's certificate (and its name): for testing against self-signed servers only. Named so
    // that it's never set by accident.
    bool insecure_skip_verification = false;

    // The ALPN protocols to offer (e.g. {"http/1.1"}), in order of preference. Empty offers none.
    std::vector<std::string> alpn{};
};

namespace detail
{

struct client_state;
struct server_state;
struct connection;

}

// client_context holds what a client's connections share: the trusted certificate authorities (loaded once), a cache of
// sessions (to resume them), and the policy. It's cheap to copy, as copies share it.
class client_context
{
public:
    // make loads the trusted certificate authorities, failing if trusted_ca_file can't be read.
    static std::expected<client_context, std::error_code> make(const client_options& options = {});

private:
    friend class stream;

    explicit client_context(std::shared_ptr<detail::client_state> state) noexcept;

    std::shared_ptr<detail::client_state> state;
};

// server_options configures a server_context.
struct server_options
{
    // PEM files: the server's certificate (followed by any intermediate certificates), and its private key.
    std::filesystem::path certificate_chain{};
    std::filesystem::path private_key{};

    // The ALPN protocols the server speaks, in order of preference: the first the client also offers is chosen. A
    // client that offers none of them is still served (without ALPN).
    std::vector<std::string> alpn = {"http/1.1"};
};

// server_context holds what a server's connections share: its certificate and key, and the policy. It's cheap to copy,
// as copies share it.
class server_context
{
public:
    // make loads the certificate chain and private key, failing if either can't be read.
    static std::expected<server_context, std::error_code> make(const server_options& options);

private:
    friend class stream;

    explicit server_context(std::shared_ptr<detail::server_state> state) noexcept;

    std::shared_ptr<detail::server_state> state;
};

// stream is a TLS connection over a TCP socket, which it owns: it reads and writes the application's data.
//
// A read and a write may run at once (e.g. on different threads), as with a socket; and as with a socket, only one of
// each. The socket's deadlines and timeouts (see socket()) limit them.
//
// Errors are the socket's, or TLS's (in category()): e.g. an alert from the peer, a certificate that couldn't be
// verified, or the connection ending without the peer's close_notify (stream_truncated). The peer's close_notify ends
// the stream (io::errc::end_of_stream). Both compare equal to io::status_condition::closed.
class stream final
    : public io::reader
    , public io::writer
{
public:
    // connect runs a client's handshake over conn, with host (the server's name, for SNI, and which its certificate
    // must have), until deadline.
    static coro::task<std::expected<stream, std::error_code>>
    connect(tcp_socket conn, const client_context& ctx, std::string host, io::clock::time_point deadline);

    // accept runs a server's handshake over conn, until deadline.
    static coro::task<std::expected<stream, std::error_code>>
    accept(tcp_socket conn, const server_context& ctx, io::clock::time_point deadline);

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

    // close sends close_notify (telling the peer that nothing was cut short), and closes the socket.
    coro::task<> close();

    // alpn is the protocol the handshake agreed on: empty if none.
    [[nodiscard]] std::string_view alpn() const noexcept;

    // socket is the TCP socket the stream runs over: e.g. for its deadlines and timeouts. Don't read or write it.
    [[nodiscard]] tcp_socket& socket() noexcept;

    [[nodiscard]] int native_handle() const noexcept override;

private:
    explicit stream(std::unique_ptr<detail::connection> conn) noexcept;

    // NOTE: on the heap, so that the socket (whose reads and writes the scheduler may be waiting on) doesn't move with
    //       the stream.
    std::unique_ptr<detail::connection> conn;
};

// errc are TLS's errors, besides its alerts (whose codes, e.g. 42 for bad_certificate, are also in category()).
enum class errc : std::uint16_t
{
    // the peer closed the connection without close_notify, so what was read may have been cut short.
    stream_truncated = 256,
    // a handshake, or an alert, that wasn't TLS (e.g. the peer isn't speaking TLS).
    protocol_error = 257,
};

// category is TLS's errors: alerts (by their codes in the TLS RFCs), and errc.
[[nodiscard]] const std::error_category& category() noexcept;

[[nodiscard]] std::error_code make_error_code(errc err) noexcept;
}

template<>
struct std::is_error_code_enum<net::tls::errc> : std::true_type
{};
