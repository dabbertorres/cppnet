#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <spdlog/logger.h>

#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http2.hpp"
#include "http/http2_connection.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/scheduler.hpp"

// HTTP/2 in http::client (see docs/plans/http2.md): the client's pool's, not for applications.
namespace net::http::detail
{

// http2_client_stream is a request sent on a client's HTTP/2 connection, and its response as it arrives.
class http2_client_stream final : public http2_stream_base
{
public:
    using http2_stream_base::http2_stream_base;

    // Guarded by the connection's lock.
    bool               head_received = false;
    status             status_code   = status::NONE;
    net::http::headers headers;
    bool               processed = false; // not refused (it may not be retried)

    // for sending the request's head, reused.
    std::vector<std::string>  names;
    std::vector<hpack::field> fields;
    std::string               authority;
    std::string               path;
};

// http2_client_options are what a client's HTTP/2 connections are made with: client_options'.
struct http2_client_options
{
    std::chrono::milliseconds response_header_timeout{};
    std::chrono::milliseconds io_timeout{};
    std::chrono::milliseconds idle_timeout{};

    http2::session_config session{};
};

// owned_connection holds what an http2_client_connection owns, before its base (which refers to the connection) is
// made.
struct owned_connection
{
    std::unique_ptr<connection>     owned;
    std::shared_ptr<spdlog::logger> log;
};

// http2_client_connection is an HTTP/2 connection that a client's requests share, each on a stream of its own.
class http2_client_connection final
    : private owned_connection // NOTE: first, so it's made before the base
    , public http2_connection_base
    , public std::enable_shared_from_this<http2_client_connection>
{
public:
    // closed is called (with the connection) once it has ended; freed whenever a stream is done (so another may be
    // sent).
    struct callbacks
    {
        std::function<void(http2_client_connection&)> closed;
        std::function<void()>                         freed;
    };

    http2_client_connection(io::scheduler*              scheduler,
                            std::unique_ptr<connection> conn,
                            const http2_client_options& options,
                            callbacks                   on);

    // start runs the connection, as one of the scheduler's tasks, returning false if the scheduler's stopping.
    bool start();

    // reserve reserves a stream for a request, which its send() takes, if the connection's open and has one free
    // (counting those reserved already), returning whether it did. Without one, send() may fail with refused_stream.
    [[nodiscard]] bool reserve();

    // idle is whether it has no streams open.
    [[nodiscard]] bool idle();

    // send sends a request, returning the response once its head has arrived. It fails with refused_stream (in
    // http2::category()) if the server didn't process the request, so it may be sent again.
    coro::task<std::expected<client_response, std::error_code>> send(client_request& request);

    // close ends the connection (GOAWAY), failing the streams still open.
    void close();

    // done is a response's body being done with its stream.
    void done(http2::stream_id id);

protected:
    // the session's events, and the base's hooks: protected, as they're declared.
    void on_headers(http2::stream_id              id,
                    http2::block_kind             kind,
                    std::span<const hpack::field> fields,
                    bool                          end_stream) override;
    void on_goaway(http2::stream_id last, std::error_code err, std::string_view debug) override;
    void on_ping_ack(std::span<const std::byte, 8> opaque) override;

    std::optional<io::clock::time_point> check_connection(io::clock::time_point now) override;
    void                                 maybe_end() override;

private:
    // open opens a stream for request, sending its head (holding the lock).
    std::expected<std::shared_ptr<http2_client_stream>, std::error_code> open(const client_request& request);

    // send_body sends the request's body on the stream, returning the error if it couldn't be.
    coro::task<std::error_code> send_body(client_request& request, http2_client_stream& stream);

    // wait_for_head waits for the response's head, returning the response (with its body), or why it didn't come.
    coro::task<std::expected<client_response, std::error_code>>
    wait_for_head(std::shared_ptr<http2_client_stream> stream, std::optional<std::error_code> body_error);

    static coro::task<> run_owned(std::shared_ptr<http2_client_connection> self);

    http2_client_options options;
    callbacks            on;

    // Guarded by mu.
    std::unordered_map<http2::stream_id, std::shared_ptr<http2_client_stream>> streams;
    std::size_t                                                                reserved    = 0;
    bool                                                                       peer_goaway = false;
    std::optional<io::clock::time_point>                                       idle_since  = io::clock::now();
};

}
