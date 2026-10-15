#include "http/server.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "exception.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/http2.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace
{

// The most of an unread request body to read and discard, rather than closing the connection.
constexpr std::size_t max_discard_bytes = std::size_t{64} * 1'024;

// discard_body reads and drops the rest of body, returning whether it reached the end of it.
net::coro::task<bool> discard_body(net::io::reader& body)
{
    std::array<std::byte, 4'096> scratch{};
    std::size_t                  total = 0;

    while (total <= max_discard_bytes)
    {
        auto res = co_await body.read(scratch);
        total += res.count;

        if (res.err) co_return res.err == net::io::status_condition::closed;
    }

    co_return false;
}

}

namespace
{

using namespace std::chrono_literals;

// linger_before_close reads (and drops) what the client is still sending, for a little while, after an error response.
//
// Closing a socket with unread input makes the kernel reset the connection, and a reset can discard the response
// before the client has read it: e.g. a 431 sent while the client is still sending its oversized headers.
net::coro::task<> linger_before_close(net::tcp_socket& conn) noexcept
{
    constexpr auto        linger_time = 2s;
    constexpr std::size_t max_drain   = std::size_t{256} * 1'024;

    conn.shutdown_write();
    conn.set_read_deadline(net::io::clock::now() + linger_time);

    std::array<std::byte, 4'096> buf{};
    std::size_t                  drained = 0;

    while (drained < max_drain)
    {
        auto res = co_await conn.read(buf);
        drained += res.count;
        if (res.err) break;
    }
}

// decode_error_status is the response to a request that couldn't be decoded.
net::http::status decode_error_status(std::error_condition err) noexcept
{
    using enum net::http::status;

    if (err == std::errc::value_too_large) return REQUEST_HEADER_FIELDS_TOO_LARGE;
    if (err == std::errc::not_supported) return NOT_IMPLEMENTED;
    return BAD_REQUEST;
}

}

namespace net::http
{

using namespace std::string_view_literals;

server::server(io::scheduler* scheduler, request_handler handler, const server_config& cfg)
    : listener{scheduler, cfg.host, cfg.port, network::tcp}
    , is_serving{false}
    , handler{std::move(handler)}
    , logger{cfg.logger}
    , scheduler{scheduler}
    , max_header_bytes{cfg.max_header_bytes}
    , header_read_timeout{cfg.header_read_timeout}
    , max_pending_connections{cfg.max_pending_connections}
{}

server::~server() { close(); }

void server::close()
{
    if (is_serving.exchange(false, std::memory_order::acq_rel))
    {
        listener.shutdown();
        logger->flush();
    }
}

coro::task<> server::serve()
{
    if (is_serving.exchange(true, std::memory_order::acq_rel))
    {
        throw exception{"already serving"};
    }

    listener.listen(max_pending_connections);

    while (is_serving.load(std::memory_order::acquire))
    {
        logger->trace("waiting for connection");

        auto accepted = co_await listener.accept();
        if (!accepted.has_value())
        {
            // the server was closed, or the scheduler stopped.
            if (accepted.error() == std::errc::operation_canceled) co_return;

            logger->error("failed to accept connection: {}", accepted.error().message());
            continue;
        }

        try
        {
            logger->trace("connection accepted: {} -> {}", accepted->remote_addr(), accepted->local_addr());
        }
        catch (const std::exception& ex)
        {
            // the connection may already be gone.
            logger->debug("accepted connection: {}", ex.what());
        }

        if (!scheduler->spawn(serve_connection(std::move(*accepted)))) co_return;
    }
}

coro::task<> server::serve_connection(tcp_socket conn) noexcept
{
    try
    {
        // Kept for the whole connection: anything read past the end of one request (e.g. a pipelined request) belongs
        // to the next.
        io::buffered_reader reader{&conn};
        io::buffered_writer writer{&conn};

        while (is_serving.load(std::memory_order::acquire) && conn.valid())
        {
            logger->trace("decoding request");

            request_decoder decode = nullptr;
            switch (0) // TODO: determine version of HTTP before full decoding
            {
            case 1: decode = http11::request_decode; break;
            case 2: decode = http2::request_decode; break;

            default: decode = http11::request_decode; break;
            }

            // Only the headers are read under the deadline: how long the handler takes to read the body is up to it.
            if (header_read_timeout > header_read_timeout.zero())
                conn.set_read_deadline(io::clock::now() + header_read_timeout);

            auto req_result = co_await decode(&reader, max_header_bytes);

            conn.set_read_deadline(std::nullopt);

            if (!req_result.has_value())
            {
                auto err = req_result.error();
                if (err == io::status_condition::closed)
                {
                    logger->debug("connection closed");
                }
                else if (err == std::errc::timed_out)
                {
                    logger->debug("timed out waiting for request headers");
                }
                else
                {
                    logger->debug("request decoding error: {} '{}'", err.value(), err.message());

                    // the rest of the connection can't be trusted to be where the next request starts, so close it.
                    server_response resp{.version = {.major = 1, .minor = 1}, .body = &writer};
                    resp.headers.set("Connection"sv, "close"sv);

                    response_writer rw{&writer, &resp, http11::response_encode};
                    co_await rw.send(decode_error_status(err), 0);
                    co_await writer.flush();
                    co_await linger_before_close(conn);
                }

                break;
            }

            logger->trace("request decoded");

            auto& req = req_result.value();
            logger->trace("request {} {} as HTTP/{}.{}",
                          method_string(req.method),
                          req.target.path,
                          req.version.major,
                          req.version.minor);

            // Only HTTP/1.x is implemented. In particular, http2::response_encode is a stub.
            const bool supported = req.version.major == 1;

            server_response resp{
                .version = supported ? req.version : protocol_version{.major = 1, .minor = 1},
                .body    = &writer,
            };

            response_writer rw{&writer, &resp, http11::response_encode};

            if (!supported)
            {
                logger->trace("unsupported http version: {}.{}", req.version.major, req.version.minor);
                co_await rw.send(status::HTTP_VERSION_NOT_SUPPORTED, 0);
                co_await writer.flush();
                co_await linger_before_close(conn);
                break;
            }

            if (auto upgrade_to = upgrade_to_protocol(req); !upgrade_to.empty())
            {
                logger->trace("upgrading to protocol: {}", upgrade_to);
                resp.headers.set("Upgrade"sv, upgrade_to);
                resp.headers.set("Connection"sv, "upgrade"sv);
                co_await rw.send(status::SWITCHING_PROTOCOLS, 0);
            }
            else
            {
                logger->trace("calling handler");

                // NOTE: a co_await can't be in a catch block, so a failure is responded to after it.
                bool failed = false;
                try
                {
                    co_await handler(req, rw);
                }
                catch (const std::exception& ex)
                {
                    logger->error("handler for {} {} threw: {}", method_string(req.method), req.target.path, ex.what());
                    failed = true;
                }
                catch (...)
                {
                    logger->error("handler for {} {} threw", method_string(req.method), req.target.path);
                    failed = true;
                }

                if (failed)
                {
                    // whatever the handler sent of the response can't be finished, so the connection is closed.
                    if (rw.status_code() == status::NONE)
                    {
                        resp.headers = {};
                        resp.headers.set("Connection"sv, "close"sv);
                        co_await rw.send(status::INTERNAL_SERVER_ERROR, 0);
                    }

                    co_await writer.flush();
                    co_await linger_before_close(conn);
                    break;
                }

                if (rw.status_code() == status::NONE) co_await rw.send(status::OK, 0);
            }

            logger->trace("flushing writer");
            co_await writer.flush();
            logger->trace("response sent");

            // Whatever of the body the handler didn't read must be skipped, or it would be parsed as the next request.
            if (!co_await discard_body(*req.body))
            {
                logger->debug("closing connection rather than discarding the rest of the request body");
                break;
            }
        }
    }
    catch (const std::exception& ex)
    {
        logger->error("fatal exception in connection handler: {}", ex.what());
    }

    logger->trace("connection closing");

    // The kernel still sends anything already written, without lingering here.
    conn.close(false);
}

std::string_view server::upgrade_to_protocol(const server_request& req) const noexcept
{
    std::string_view upgrade_to;
    for (auto protocol : req.headers.get_list("Upgrade"sv))
    {
        if (is_protocol_supported(protocol))
        {
            upgrade_to = protocol;
            break;
        }
    }

    return upgrade_to;
}

bool server::is_protocol_supported(std::string_view protocol) const noexcept
{
    if (protocol == "HTTP/1.1") return true;

    return false;
}

}
