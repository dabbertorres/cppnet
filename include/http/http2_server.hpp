#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <stop_token>

#include <spdlog/logger.h>

#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/http2_session.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/scheduler.hpp"

// HTTP/2 in http::server (see docs/plans/http2.md): the server's, and response_writer's, not for applications.
namespace net::http::detail
{

// http2_options are what an HTTP/2 connection is served with: server_config's.
struct http2_options
{
    // How long the preface may take, and a connection may be idle (with no streams) before it's closed; and how long
    // a stream's body may take to arrive, or a response to be sent, without progress, before the stream's reset. 0 is
    // no limit.
    std::chrono::seconds header_read_timeout{};
    std::chrono::seconds body_read_timeout{};
    std::chrono::seconds write_timeout{};

    http2::session_config session{};
};

// serve_http2 serves an HTTP/2 connection with handler, until it ends: when the client closes it, or it fails, or
// stop is requested (which ends it gracefully: GOAWAY, then once its streams are done). already_read is what was read
// of the connection before (e.g. the preface, read to choose HTTP/2), to be received first.
coro::task<> serve_http2(io::scheduler*             scheduler,
                         connection&                conn,
                         std::span<const std::byte> already_read,
                         const http2_options&       options,
                         const request_handler&     handler,
                         spdlog::logger&            logger,
                         std::stop_token            stop);

// http2_ops are what response_writer does with the server's HTTP/2 streams.
extern const http2_stream_ops http2_ops;
}
