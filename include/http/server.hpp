#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include "coro/async_scope.hpp"
#include "coro/task.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/discard_writer.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace net::http
{

using namespace std::chrono_literals;

struct server_config
{
    std::string host{};
    std::string port             = "8080";
    std::size_t max_header_bytes = 8'192;
    // How long to wait for a request's headers, from when the server starts waiting for them, so it includes waiting
    // for the next request on a kept-alive connection. The connection is closed once it passes. 0 disables it.
    std::chrono::seconds header_read_timeout = 5s;
    // How long reading a request's body (by the handler, or skipping what it didn't read), and writing a response, may
    // each go without progress. The connection is closed once either passes. 0 disables them.
    std::chrono::seconds body_read_timeout = 60s;
    std::chrono::seconds write_timeout     = 60s;
    // The most connections served at once: once that many are open, the server waits for one to close before
    // accepting another. 0 doesn't limit them.
    std::size_t                     max_connections         = 0;
    std::uint16_t                   max_pending_connections = 512;
    std::shared_ptr<spdlog::logger> logger                  = spdlog::default_logger();
};

class server
{
public:
    // handler is called for each request. If it returns without sending a response, an empty 200 OK is sent. If it
    // throws, a 500 Internal Server Error is sent (if nothing was yet), and the connection is closed.
    server(io::scheduler* scheduler, request_handler handler, const server_config& cfg = server_config{});

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

    // set_head_deadline sets how long conn may take to send the next request's head (see header_read_timeout).
    void set_head_deadline(tcp_socket& conn) const;

    // start_response readies the connection's response for the request it has just decoded, with no_body for its body
    // if it's to a HEAD request.
    static void start_response(connection_state& state, io::discard_writer& no_body);

    // The ways a connection ends other than the client closing it, which close it.
    coro::task<> end_after_decode_error(tcp_socket& conn, connection_state& state, std::error_condition err);
    coro::task<> end_after_handler_failure(tcp_socket&        conn,
                                           connection_state&  state,
                                           response_writer&   rw,
                                           std::exception_ptr failure);

    // respond_and_close sends a response of just code (closing the connection), and closes it.
    coro::task<> respond_and_close(tcp_socket& conn, connection_state& state, status code);

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

    // open_connections counts the connections being served, for max_connections (waited on, with the scheduler).
    std::atomic<std::uint64_t> open_connections = 0;

    // connections are the sockets being served, for close() to end. Each removes itself before closing its socket.
    std::mutex                      connections_mu;
    std::unordered_set<tcp_socket*> connections;

    // running counts serve() and the connections (which are the scheduler's tasks), for the destructor to wait for.
    coro::async_scope running;
};

}
