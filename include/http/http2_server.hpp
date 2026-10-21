#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>

#include <spdlog/logger.h>

#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/http2_config.hpp"
#include "http/priority.hpp"
#include "http/request.hpp"
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

// stream_priority is the priority of the HTTP/2 stream a request (its handler's) came on, as the server has it now
// (see docs/plans/http2.md, Priorities), or nullopt if it didn't come over HTTP/2 (or its body was replaced). For
// tests.
[[nodiscard]] std::optional<priority> stream_priority(const server_request& req);

// wait_queued_streams waits until at least n streams of the HTTP/2 connection a request came on wait in its scheduler
// for their turns to send (true), or the connection's ending, or the request didn't come over HTTP/2 (false). For
// tests: e.g. to know that every response has queued (with no window to send) before letting them go.
coro::task<bool> wait_queued_streams(const server_request& req, std::size_t n);

}
