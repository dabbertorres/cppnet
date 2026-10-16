#include "http/server.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "exception.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/discard_writer.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace
{

using namespace std::chrono_literals;

// The most of an unread request body to read and discard, rather than closing the connection.
constexpr std::size_t max_discard_bytes = std::size_t{64} * 1'024;

// How much a connection reads, and writes, at once.
constexpr std::size_t buffer_size = std::size_t{16} * 1'024;

// current_date is the Date header's value for a response sent now (RFC 9110, section 6.6.1: a server with a clock
// sends one). It's formatted at most once a second, on each thread.
std::string current_date()
{
    thread_local std::chrono::sys_seconds formatted_at{};
    thread_local std::string              formatted;

    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    if (now != formatted_at)
    {
        formatted    = net::http::format_date(now);
        formatted_at = now;
    }

    return formatted;
}

// flush_then_read reads from a connection, first sending the responses written to it so far. So, a server holds its
// responses until it has answered the requests it has already read: a batch of pipelined requests is answered with one
// write, rather than one each, and a request that isn't pipelined is answered as the server waits for the next.
//
// NOTE: so a response to a pipelined request also waits for the requests read along with it to be handled.
class flush_then_read final : public net::io::reader
{
public:
    flush_then_read(net::io::reader* conn, net::io::buffered_writer* writer) noexcept
        : conn{conn}
        , writer{writer}
    {}

    net::coro::task<net::io::result> read(std::span<std::byte> data) override
    {
        if (writer->size() == 0) return conn->read(data);
        return flush_and_read(data);
    }

    [[nodiscard]] int native_handle() const noexcept override { return conn->native_handle(); }

private:
    net::coro::task<net::io::result> flush_and_read(std::span<std::byte> data)
    {
        // the client can't be answered any more, which ends the connection as its closing would.
        if (auto flushed = co_await writer->flush(); flushed.err)
            co_return net::io::result{.err = make_error_condition(net::io::status_condition::closed)};

        co_return co_await conn->read(data);
    }

    net::io::reader*          conn;
    net::io::buffered_writer* writer;
};

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

        // responses are written once each is finished, or a batch of them is (see flush_then_read), so there's nothing
        // to gain from the kernel holding a write back until the last is acknowledged, and it can delay the response.
        if (auto set = accepted->set_nodelay(); !set.has_value())
            logger->debug("failed to set TCP_NODELAY: {}", set.error().message());

        if (!scheduler->spawn(serve_connection(std::move(*accepted)))) co_return;
    }
}

coro::task<> server::serve_connection(tcp_socket conn) noexcept
{
    try
    {
        // Kept for the whole connection: anything read past the end of one request (e.g. a pipelined request) belongs
        // to the next.
        io::buffered_writer writer{&conn, buffer_size};
        flush_then_read     input{&conn, &writer};
        io::buffered_reader reader{&input, buffer_size};

        // NOTE: not conn.valid(), a syscall per request: a socket's pending error fails the next read or write anyway.
        while (is_serving.load(std::memory_order::acquire))
        {
            auto state = co_await serve_request(conn, reader, writer);
            if (state == serve_state::close) break;
        }

        co_await writer.flush();
    }
    catch (const std::exception& ex)
    {
        logger->error("fatal exception in connection handler: {}", ex.what());
    }

    logger->trace("connection closing");

    // The kernel still sends anything already written, without lingering here.
    conn.close(false);
}

coro::task<server::serve_state>
server::serve_request(tcp_socket& conn, io::buffered_reader& reader, io::buffered_writer& writer) noexcept
{
    logger->trace("decoding request");

    // Only the headers are read under the deadline: how long the handler takes to read the body is up to it.
    if (header_read_timeout > std::chrono::seconds::zero())
        conn.set_read_deadline(io::clock::now() + header_read_timeout);

    // NOTE: HTTP/2 (parked) would be told apart here by its connection preface ("PRI * HTTP/2.0"), which HTTP/1.1
    // decoding rejects as not_supported, so it gets a 501 Not Implemented.
    auto req_result = co_await http11::request_decode(&reader, max_header_bytes);

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
            server_response resp{
                .version = {.major = 1, .minor = 1},
                .headers = {},
                .body    = &writer
            };
            resp.headers.set("Connection"sv, "close"sv);
            resp.headers.set("Date"sv, current_date());

            response_writer rw{&writer, &resp, http11::response_encode};
            co_await rw.send(decode_error_status(err), 0);
            co_await writer.flush();
            co_await linger_before_close(conn);
        }

        co_return serve_state::close;
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

    // A response to HEAD has the headers a GET would, but no body: what the handler writes as one is dropped.
    io::discard_writer no_body;

    server_response resp{
        .version = supported ? req.version : protocol_version{.major = 1, .minor = 1},
        .headers = {},
        .body    = req.method == request_method::HEAD ? static_cast<io::writer*>(&no_body) : &writer,
    };
    resp.headers.set("Date"sv, current_date());

    response_writer rw{&writer, &resp, http11::response_encode};

    if (!supported)
    {
        logger->trace("unsupported http version: {}.{}", req.version.major, req.version.minor);
        co_await rw.send(status::HTTP_VERSION_NOT_SUPPORTED, 0);
        co_await writer.flush();
        co_await linger_before_close(conn);
        co_return serve_state::close;
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
                resp.headers.set("Date"sv, current_date());
                co_await rw.send(status::INTERNAL_SERVER_ERROR, 0);
            }

            co_await writer.flush();
            co_await linger_before_close(conn);
            co_return serve_state::close;
        }

        if (rw.status_code() == status::NONE) co_await rw.send(status::OK, 0);

        // the client would wait for the rest of the body, so it's told there's no more by closing the connection.
        if (rw.body_remaining() > 0)
        {
            logger->warn("handler for {} {} wrote {} bytes less than its Content-Length",
                         method_string(req.method),
                         req.target.path,
                         rw.body_remaining());
            co_await writer.flush();
            co_return serve_state::close;
        }
    }

    // NOTE: the response is sent when the server next reads from the connection (see flush_then_read).

    // Whatever of the body the handler didn't read must be skipped, or it would be parsed as the next request.
    if (!co_await discard_body(*req.body))
    {
        logger->debug("closing connection rather than discarding the rest of the request body");
        co_return serve_state::close;
    }

    co_return serve_state::keep_alive;
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

bool server::is_protocol_supported(std::string_view protocol) const noexcept { return protocol == "HTTP/1.1"; }
}
