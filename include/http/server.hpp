#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include "coro/async_scope.hpp"
#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/http2_server.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/discard_writer.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"

#ifdef NET_HAS_TLS
#    include "tls/tls.hpp"
#endif

namespace net::http
{

struct server_config
{
    // Where to listen, for the constructors that don't take a listener.
    std::string host{};
    std::string port             = "8080";
    std::size_t max_header_bytes = 8'192;
    // How long to wait for a request's headers, from when the server starts waiting for them, so it includes waiting
    // for the next request on a kept-alive connection. The connection is closed once it passes. 0 disables it.
    std::chrono::seconds header_read_timeout = std::chrono::seconds{5};
    // How long reading a request's body (by the handler, or skipping what it didn't read), and writing a response, may
    // each go without progress. The connection is closed once either passes. 0 disables them.
    std::chrono::seconds body_read_timeout = std::chrono::seconds{60};
    std::chrono::seconds write_timeout     = std::chrono::seconds{60};
    // The most connections served at once: once that many are open, the server waits for one to close before
    // accepting another. 0 doesn't limit them. (512 is nginx's worker_connections default.)
    std::size_t   max_connections         = 512;
    std::uint16_t max_pending_connections = 512;

    // Serve HTTP/2 as well as HTTP/1.1: over TLS when the client agrees on h2 with ALPN, and over cleartext when a
    // client starts with HTTP/2's preface (prior knowledge). Its streams are limited by the timeouts above, as
    // requests are (a stream that times out is reset, rather than the connection closed), and a connection without
    // streams is closed after header_read_timeout.
    bool http2 = true;

    // The most streams (requests) an HTTP/2 client may have open on a connection at once (nginx's default).
    std::uint32_t http2_max_concurrent_streams = 100;

    std::shared_ptr<spdlog::logger> logger = spdlog::default_logger();

#ifdef NET_HAS_TLS
    // Serve HTTPS: each connection starts with a TLS handshake, with this context's certificate and key (see
    // tls::server_context::make()), which has header_read_timeout to finish. The server offers h2 (if http2 is on) and
    // http/1.1 with ALPN, whatever the context's options say.
    std::optional<tls::server_context> tls{};
#endif
};

class server
{
public:
    // The server listens from when it's constructed, queuing connections (up to max_pending_connections) until serve()
    // accepts them: on cfg's host and port, or with listener (e.g. from listener::make(), which reports a failure to
    // listen as an error, where these constructors throw std::system_error).
    //
    // handler is called for each request. If it returns without sending a response, an empty 200 OK is sent. If it
    // throws, a 500 Internal Server Error is sent (if nothing was yet), and the connection is closed.
    server(io::scheduler* scheduler, net::listener listener, request_handler handler, const server_config& cfg = {});

    server(io::scheduler* scheduler, net::listener listener, router&& handler, const server_config& cfg = {})
        : server{scheduler, std::move(listener), make_handler(std::move(handler)), cfg}
    {}

    server(io::scheduler*       scheduler,
           request_handler      handler,
           const server_config& cfg =
               server_config{
    })
        : server{scheduler, net::listener{scheduler, cfg.host, cfg.port, network::tcp}, std::move(handler), cfg}
    {}

    server(io::scheduler* scheduler, router&& handler, const server_config& cfg = server_config{})
        : server{scheduler, make_handler(std::move(handler)), cfg}
    {}

    server(const server&)            = delete;
    server& operator=(const server&) = delete;

    server(server&&)            = delete;
    server& operator=(server&&) = delete;

    // Closes the server, and blocks until serve() and the connections have finished: so it must not be destroyed
    // from one of the scheduler's tasks, which might be one of them (or one a worker it needs is running).
    ~server();

    // close stops accepting connections, and ends the open ones: those waiting for a request close, and the others
    // once their response is sent (reads of a request's body find its end).
    void close();

    // serve accepts and serves connections, until the server is closed (or the scheduler stopped).
    coro::task<> serve();

    // local_port is the port the server listens on: e.g. the one chosen for it, if it was configured with port "0".
    [[nodiscard]] std::uint16_t local_port() const { return listener.local_port(); }

private:
    // connection_state is what's kept for a connection from one request to the next (see server.cpp).
    struct connection_state;

    // room_for_connection waits until fewer than max_connections are open, returning false if the scheduler stops.
    coro::task<bool> room_for_connection();

    // serve_connection serves conn's requests, holding a lease on running (so the server isn't destroyed meanwhile).
    coro::task<> serve_connection(tcp_socket conn, coro::async_scope::lease held) noexcept;

    // open_connection makes conn the connection to serve: over TLS, after a handshake (none, if it fails).
    coro::task<std::optional<detail::connection>> open_connection(tcp_socket conn);

#ifdef NET_HAS_TLS
    // accept_tls runs conn's TLS handshake with ctx.
    coro::task<std::optional<detail::connection>> accept_tls(tcp_socket conn, tls::server_context ctx);
#endif

    // serve_requests reads and responds to conn's HTTP/1.x requests, until it closes (or should be); or if it starts
    // with HTTP/2's preface (and http2 is on), serves it as HTTP/2.
    coro::task<> serve_requests(detail::connection& conn);

    // serve_http2 serves conn as HTTP/2, already_read being what was read of it to tell.
    coro::task<> serve_http2(detail::connection& conn, std::span<const std::byte> already_read);

    // head_deadline is how long a connection may take to send the next request's head, or finish its TLS handshake
    // (see header_read_timeout).
    [[nodiscard]] std::optional<io::clock::time_point> head_deadline() const;

    // start_response readies the connection's response for the request it has just decoded, with no_body for its body
    // if it's to a HEAD request.
    static void start_response(connection_state& state, io::discard_writer& no_body);

    // The ways a connection ends other than the client closing it, which close it.
    coro::task<> end_after_decode_error(detail::connection& conn, connection_state& state, std::error_code err);
    coro::task<> end_after_handler_failure(detail::connection& conn,
                                           connection_state&   state,
                                           response_writer&    rw,
                                           std::exception_ptr  failure);

    // respond_and_close sends a response of just code (closing the connection), and closes it.
    coro::task<> respond_and_close(detail::connection& conn, connection_state& state, status code);

    net::listener                   listener;
    std::atomic_bool                is_serving;
    request_handler                 handler;
    std::shared_ptr<spdlog::logger> logger;
    io::scheduler*                  scheduler;

    std::size_t          max_header_bytes;
    std::chrono::seconds header_read_timeout;
    std::chrono::seconds body_read_timeout;
    std::chrono::seconds write_timeout;
    std::size_t          max_connections;
    std::uint16_t        max_pending_connections;

    bool                  http2_enabled;
    detail::http2_options http2_config;

    // open_connections counts the connections being served, for max_connections (waited on, with the scheduler).
    std::atomic<std::uint64_t> open_connections = 0;

#ifdef NET_HAS_TLS
    std::optional<tls::server_context> tls_ctx;
#endif

    // closing is requested by close(), to end the connections (and TLS handshakes) being served.
    std::stop_source closing;

    // running counts serve() and the connections (which are the scheduler's tasks), for the destructor to wait for.
    coro::async_scope running;
};

}
