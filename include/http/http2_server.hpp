#pragma once

#include <cstddef>
#include <span>
#include <stop_token>

#include <spdlog/logger.h>

#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/http2_config.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/scheduler.hpp"

// HTTP/2 in http::server (see docs/plans/http2.md): the server's, and response_writer's, not for applications.
namespace net::http::detail
{

// serve_http2 serves an HTTP/2 connection with handler, until it ends: when the client closes it, or it fails, or
// stop is requested (which ends it gracefully: GOAWAY, then once its streams are done). abort stops the handlers'
// tokens (server_request::stop): those running, and those of streams started after. already_read is what was read of
// the connection before (e.g. the preface, read to choose HTTP/2), to be received first.
coro::task<> serve_http2(io::scheduler*             scheduler,
                         connection&                conn,
                         std::span<const std::byte> already_read,
                         const http2_options&       options,
                         const request_handler&     handler,
                         spdlog::logger&            logger,
                         std::stop_token            stop,
                         std::stop_token            abort);

// http2_ops are what response_writer does with the server's HTTP/2 streams.
extern const http2_stream_ops http2_ops;
}
