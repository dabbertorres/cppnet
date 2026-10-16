#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

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
    std::chrono::seconds            header_read_timeout     = 5s;
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

    server(server&&) noexcept;
    server& operator=(server&&) noexcept;

    ~server();

    void         close();
    coro::task<> serve();

    // local_port is the port the server listens on: e.g. the one chosen for it, if it was configured with port "0".
    [[nodiscard]] std::uint16_t local_port() const { return listener.local_port(); }

private:
    // connection_state is what's kept for a connection from one request to the next (see server.cpp).
    struct connection_state;

    coro::task<> serve_connection(tcp_socket conn) noexcept;

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

    std::string_view upgrade_to_protocol(const server_request& req) const noexcept;
    bool             is_protocol_supported(std::string_view protocol) const noexcept;

    net::listener                   listener;
    std::atomic_bool                is_serving;
    request_handler                 handler;
    std::shared_ptr<spdlog::logger> logger;
    io::scheduler*                  scheduler;

    std::size_t          max_header_bytes;
    std::chrono::seconds header_read_timeout;
    std::uint16_t        max_pending_connections;
};

}
