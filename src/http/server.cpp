#include "http/server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "exception.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
#include "http/http2_server.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/discard_writer.hpp"
#include "io/flush_before_read.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "tcp.hpp"

#ifdef NET_HAS_TLS
#    include "tls/tls.hpp"
#endif

namespace
{

using namespace std::chrono_literals;
using namespace std::string_view_literals;

// The most of an unread request body to read and discard, rather than closing the connection.
constexpr std::size_t max_discard_bytes = std::size_t{64} * 1'024;

// How much a connection reads, and writes, at once.
constexpr std::size_t buffer_size = std::size_t{16} * 1'024;

// current_date is the Date header's value for a response sent now (RFC 9110, section 6.6.1: a server with a clock
// sends one). It's formatted at most once a second, on each thread, so the view is only valid until the thread next
// calls it (e.g. after a co_await, which may continue on another).
std::string_view current_date()
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

// discard_rest finishes discard_body, once reading (which needed a coroutine) does, total having been read before it.
net::coro::task<bool> discard_rest(net::io::reader&                 body,
                                   std::span<std::byte>             scratch,
                                   net::coro::task<net::io::result> reading,
                                   std::size_t                      total)
{
    auto res = co_await std::move(reading);
    while (true)
    {
        total += res.count;
        if (res.err) co_return res.err == net::io::errc::end_of_stream;
        if (total > max_discard_bytes) co_return false;

        res = co_await body.read(scratch);
    }
}

// discard_body reads (into scratch) and drops the rest of body, returning whether it reached the end of it.
net::coro::task<bool> discard_body(net::io::reader& body, std::span<std::byte> scratch)
{
    std::size_t total = 0;

    // usually, there's none, or it's all buffered, so the reads don't wait: then there's no need for a coroutine (see
    // net::coro::ready_task).
    while (total <= max_discard_bytes)
    {
        auto reading = body.read(scratch);
        if (!reading.finished_without_coroutine()) return discard_rest(body, scratch, std::move(reading), total);

        auto res = std::move(reading).result();
        total += res.count;
        if (res.err) return net::coro::ready_task<bool>(res.err == net::io::errc::end_of_stream);
    }

    return net::coro::ready_task<bool>(false);
}

// linger_before_close reads (and drops) what the client is still sending, for a little while, after an error response.
//
// Closing a socket with unread input makes the kernel reset the connection, and a reset can discard the response
// before the client has read it: e.g. a 431 sent while the client is still sending its oversized headers.
net::coro::task<> linger_before_close(net::http::detail::connection& conn) noexcept
{
    constexpr auto        linger_time = 2s;
    constexpr std::size_t max_drain   = std::size_t{256} * 1'024;

    // over TLS, after close_notify: what's read isn't decrypted (it's dropped), so it's read from the socket.
    (void)co_await net::http::detail::shutdown(conn);

    auto& sock = net::http::detail::socket_of(conn);
    sock.set_read_deadline(net::io::clock::now() + linger_time);

    std::array<std::byte, 4'096> buf{};
    std::size_t                  drained = 0;

    while (drained < max_drain)
    {
        auto res = co_await sock.read(buf);
        drained += res.count;
        if (res.err) break;
    }
}

// starts_with_preface reads (into reader's buffer, without consuming it) until it can tell whether the connection
// starts with HTTP/2's preface.
net::coro::task<bool> starts_with_preface(net::io::buffered_reader& reader)
{
    const auto preface = std::as_bytes(std::span{net::http::http2::preface});
    while (true)
    {
        const auto buffered = reader.buffered();
        const auto n        = std::min(buffered.size(), preface.size());
        if (!std::ranges::equal(buffered.first(n), preface.first(n))) co_return false;
        if (n == preface.size()) co_return true;

        if (co_await reader.fill()) co_return false;
    }
}

// http2_session_config is HTTP/2's settings for a server's config: its streams' limit, and its head's, as a header list
// (whose fields count 32 more each, for at most as many fields as HTTP/1.1's); and the connection's window, which holds
// every stream's (at least 1 MiB), so bodies read one at a time don't wait for each other.
net::http::http2::session_config http2_session_config(const net::http::server_config& cfg)
{
    net::http::http2::session_config config;
    config.local.max_concurrent_streams = cfg.http2_max_concurrent_streams;
    config.local.max_header_list_size =
        static_cast<std::uint32_t>(std::min<std::size_t>(cfg.max_header_bytes + (32 * net::http::http11::max_fields),
                                                         net::http::http2::unlimited - 1));

    const auto streams       = std::uint64_t{config.local.max_concurrent_streams} * config.local.initial_window_size;
    config.connection_window = static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>(streams, std::uint64_t{1} << 20, net::http::http2::max_window));
    return config;
}

// has_close is whether fields say the connection closes after this message.
bool has_close(const net::http::headers& fields) { return fields.has_token("Connection"sv, "close"sv); }

// response_version is the version of a response to a request of version: the highest the server implements, which the
// client does too (RFC 9110, section 2.5).
net::http::protocol_version response_version(net::http::protocol_version version) noexcept
{
    return {.major = 1, .minor = version.major == 1 && version.minor == 0 ? 0U : 1U};
}

// decode_error_status is the response to a request that couldn't be decoded.
net::http::status decode_error_status(std::error_code err) noexcept
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

server::server(io::scheduler* scheduler, net::listener listener, request_handler handler, const server_config& cfg)
    : listener{std::move(listener)}
    , is_serving{false}
    , handler{std::move(handler)}
    , logger{cfg.logger}
    , scheduler{scheduler}
    , max_header_bytes{cfg.max_header_bytes}
    , header_read_timeout{cfg.header_read_timeout}
    , body_read_timeout{cfg.body_read_timeout}
    , write_timeout{cfg.write_timeout}
    , max_connections{cfg.max_connections}
    , max_pending_connections{cfg.max_pending_connections}
    , http2_enabled{cfg.http2}
    , http2_config{
          .header_read_timeout = cfg.header_read_timeout,
          .body_read_timeout   = cfg.body_read_timeout,
          .write_timeout       = cfg.write_timeout,
          .session             = http2_session_config(cfg),
      }
#ifdef NET_HAS_TLS
    , tls_ctx{cfg.tls}
#endif
{
    // NOTE: listening from now, rather than from when serve() starts (on a worker), so connections made in between are
    //       queued (up to max_pending_connections) rather than refused.
    if (!this->listener.listening()) this->listener.listen(max_pending_connections);
}

server::~server()
{
    close();

    // serve() and the connections use the server until they finish.
    running.close();
    running.join_blocking();
}

void server::close()
{
    if (!is_serving.exchange(false, std::memory_order::acq_rel)) return;

    listener.shutdown();

    // ends the connections, and TLS handshakes, being served: and any yet to start, as requesting stop runs the
    // callbacks registered after it straight away.
    closing.request_stop();

    // serve() may be waiting for a connection to close (see max_connections).
    scheduler->notify_all(open_connections);
    logger->flush();
}

coro::task<> server::serve()
{
    // the server waits for this to finish before it's destroyed.
    auto held = running.acquire();
    if (!held) co_return;

    if (is_serving.exchange(true, std::memory_order::acq_rel))
    {
        throw exception{"already serving"};
    }

    auto backoff = io::clock::duration::zero();

    while (is_serving.load(std::memory_order::acquire))
    {
        if (!co_await room_for_connection()) co_return;

        logger->trace("waiting for connection");

        auto accepted = co_await listener.accept();
        if (!accepted.has_value())
        {
            // the server was closed, or the scheduler stopped.
            if (accepted.error() == std::errc::operation_canceled) co_return;

            // e.g. too many open files: trying again straight away would fail again (and on Linux, which leaves the
            // connection queued, straight away), so it waits a little longer each time.
            backoff = std::min<io::clock::duration>(backoff == io::clock::duration::zero() ? 5ms : backoff * 2, 1s);
            logger->error("failed to accept connection: {}", accepted.error().message());

            if (co_await scheduler->sleep_until(io::clock::now() + backoff)) co_return;
            continue;
        }

        backoff = io::clock::duration::zero();

        try
        {
            logger->trace("connection accepted: {} -> {}", accepted->remote_addr(), accepted->local_addr());
        }
        catch (const std::exception& ex)
        {
            // the connection may already be gone.
            logger->debug("accepted connection: {}", ex.what());
        }

        // responses are written once each is finished, or a batch of them is (see io::flush_before_read), so there's
        // nothing to gain from the kernel holding a write back until the last is acknowledged, and it can delay the
        // response.
        if (auto set = accepted->set_nodelay(); !set.has_value())
            logger->debug("failed to set TCP_NODELAY: {}", set.error().message());

        auto conn_held = running.acquire();
        if (!conn_held) co_return;

        open_connections.fetch_add(1, std::memory_order::acq_rel);
        if (!scheduler->spawn(serve_connection(std::move(*accepted), std::move(conn_held))))
        {
            // the task was destroyed without running (as the scheduler is stopping).
            open_connections.fetch_sub(1, std::memory_order::acq_rel);
            co_return;
        }
    }
}

coro::task<bool> server::room_for_connection()
{
    while (max_connections != 0 && is_serving.load(std::memory_order::acquire))
    {
        const auto open = open_connections.load(std::memory_order::acquire);
        if (open < max_connections) co_return true;

        // the scheduler's being stopped.
        if (co_await scheduler->wait_on(open_connections, open)) co_return false;
    }

    co_return true;
}

// connection_state is what's kept for a connection from one request to the next: anything read past the end of one
// request (e.g. a pipelined request) belongs to the next; and the request, response and scratch buffer are reused, so
// that their memory (e.g. for header values) is.
struct server::connection_state
{
    explicit connection_state(detail::connection* conn)
        : writer{&detail::as_writer(*conn), buffer_size}
        , input{&detail::as_reader(*conn), &writer}
        , reader{&input, buffer_size}
    {}

    io::buffered_writer   writer;
    io::flush_before_read input;
    io::buffered_reader   reader;

    server_request  req;
    server_response resp;

    // for reading what's left of a request's body, to drop it.
    std::array<std::byte, 4'096> scratch{};
};

coro::task<> server::serve_connection(tcp_socket conn, coro::async_scope::lease /*held*/) noexcept
{
    std::optional<detail::connection> opened;

    try
    {
        // NOTE: per wait, so a slow (or stuck) client can't hold the connection for ever.
        if (body_read_timeout > std::chrono::seconds::zero()) conn.set_read_timeout(body_read_timeout);
        if (write_timeout > std::chrono::seconds::zero()) conn.set_write_timeout(write_timeout);

        opened = co_await open_connection(std::move(conn));
        if (opened)
        {
            // TLS chooses HTTP/2 with ALPN; over cleartext, serve_requests() looks for its preface.
            if (http2_enabled && detail::alpn(*opened) == "h2"sv) co_await serve_http2(*opened, {});
            else co_await serve_requests(*opened);
        }
    }
    catch (const std::exception& ex)
    {
        logger->error("fatal exception in connection handler: {}", ex.what());
    }

    logger->trace("connection closing");

    // over TLS, after close_notify.
    if (opened) co_await detail::close(*opened);

    // serve() may be waiting for a connection to close (see max_connections).
    open_connections.fetch_sub(1, std::memory_order::acq_rel);
    scheduler->notify_one(open_connections);
}

coro::task<std::optional<detail::connection>> server::open_connection(tcp_socket conn)
{
#ifdef NET_HAS_TLS
    if (tls_ctx) return accept_tls(std::move(conn), *tls_ctx);
#endif

    return coro::ready_task<std::optional<detail::connection>>(std::in_place, std::move(conn));
}

#ifdef NET_HAS_TLS
coro::task<std::optional<detail::connection>> server::accept_tls(tcp_socket conn, tls::server_context ctx)
{
    // NOTE: the handshake needs a deadline: without header_read_timeout, only close() ends one that doesn't finish.
    const auto deadline = head_deadline().value_or(io::clock::now() + std::chrono::hours{24} * 365);

    // the server decides what it speaks, whatever the context's options say.
    std::vector<std::string> protocols{"http/1.1"};
    if (http2_enabled) protocols.insert(protocols.begin(), "h2");

    auto stream = co_await tls::stream::accept(std::move(conn),
                                               std::move(ctx),
                                               deadline,
                                               closing.get_token(),
                                               std::move(protocols));
    if (!stream.has_value())
    {
        logger->debug("TLS handshake failed: {}", stream.error().message());
        co_return std::nullopt;
    }

    // (a client that agreed on a protocol the server didn't offer would speak it.)
    if (const auto protocol = stream->alpn();
        !protocol.empty() && protocol != "http/1.1"sv && !(http2_enabled && protocol == "h2"sv))
    {
        logger->debug("closing connection that agreed on {} with ALPN", protocol);
        co_await stream->close();
        co_return std::nullopt;
    }

    co_return std::optional<detail::connection>{std::in_place, std::move(*stream)};
}
#endif

coro::task<> server::serve_requests(detail::connection& conn)
{
    connection_state state{&conn};
    auto&            req  = state.req;
    auto&            resp = state.resp;

    req.tls = detail::is_tls(conn);

    // close() ends the connection: reads (e.g. waiting for the next request) find the end of the stream.
    // NOTE: unregistered (waiting for a call in progress) before the connection closes, or is served as HTTP/2.
    const auto shut_down = [&conn] noexcept { detail::socket_of(conn).shutdown_read(); };
    std::optional<std::stop_callback<decltype(shut_down)>> on_close{std::in_place, closing.get_token(), shut_down};

    // a cleartext client may start with HTTP/2's preface, knowing the server speaks it (RFC 9113, section 3.3).
    if (http2_enabled && !req.tls)
    {
        detail::socket_of(conn).set_read_deadline(head_deadline());
        if (co_await starts_with_preface(state.reader))
        {
            on_close.reset();
            co_await serve_http2(conn, state.reader.buffered());
            co_return;
        }
    }

    // NOTE: each request is served here, rather than by a coroutine of its own, which would allocate its frame for
    //       each request (as one this large isn't elided). Only the rare ways a connection ends have their own.
    // NOTE: not conn.valid(), a syscall per request: a socket's pending error fails the next read or write anyway.
    while (is_serving.load(std::memory_order::acquire))
    {
        // Only the headers are read under the deadline: how long the handler takes to read the body is up to it.
        detail::socket_of(conn).set_read_deadline(head_deadline());

        auto err = co_await http11::request_decode(&state.reader, max_header_bytes, req);

        detail::socket_of(conn).set_read_deadline(std::nullopt);

        if (err)
        {
            co_await end_after_decode_error(conn, state, err);
            break;
        }

        logger->trace("request {} {} as HTTP/{}.{}",
                      method_string(req.method),
                      req.target.path,
                      req.version.major,
                      req.version.minor);

        // Only HTTP/1.x is implemented. In particular, http2::response_encode is a stub.
        if (req.version.major != 1)
        {
            logger->trace("unsupported http version: {}.{}", req.version.major, req.version.minor);
            co_await respond_and_close(conn, state, status::HTTP_VERSION_NOT_SUPPORTED);
            break;
        }

        io::discard_writer no_body;
        start_response(state, no_body);

        response_writer rw{&state.writer, &resp, http11::response_encode};

        // NOTE: a co_await can't be in a catch block, so a failure is responded to after it.
        std::exception_ptr failure;
        try
        {
            co_await handler(req, rw);
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        if (failure)
        {
            co_await end_after_handler_failure(conn, state, rw, failure);
            break;
        }

        if (!rw.sent()) co_await rw.send(status::OK, 0);

        // ends a body sent without a Content-Length, chunked.
        if (auto finished = co_await rw.finish(); finished.err)
        {
            logger->debug("failed to end the response's body: {}", finished.err.message());
            break;
        }

        // the client would wait for the rest of the body, so it's told there's no more by closing the connection.
        // NOTE: not for HEAD, whose body isn't sent.
        if (req.method != request_method::HEAD && rw.body_remaining() > 0)
        {
            logger->warn("handler for {} {} wrote {} bytes less than its Content-Length",
                         method_string(req.method),
                         req.target.path,
                         rw.body_remaining());
            break;
        }

        // NOTE: the response is sent when the server next reads from the connection (see io::flush_before_read).

        // Whatever of the body the handler didn't read must be skipped, or it would be parsed as the next request.
        if (!co_await discard_body(*req.body, state.scratch))
        {
            logger->debug("closing connection rather than discarding the rest of the request body");
            break;
        }

        // the client, or the handler, asked for the connection to close after this response.
        if (has_close(resp.headers)) break;
    }

    co_await state.writer.flush();
}

coro::task<> server::serve_http2(detail::connection& conn, std::span<const std::byte> already_read)
{
    // HTTP/2's reader waits for as long as the connection's open: its streams, and the connection, have timeouts of
    // their own.
    auto& sock = detail::socket_of(conn);
    sock.set_read_timeout(std::nullopt);
    sock.set_read_deadline(std::nullopt);

    co_await detail::serve_http2(scheduler, conn, already_read, http2_config, handler, *logger, closing.get_token());
}

std::optional<io::clock::time_point> server::head_deadline() const
{
    if (header_read_timeout <= std::chrono::seconds::zero()) return std::nullopt;
    return io::clock::now() + header_read_timeout;
}

void server::start_response(connection_state& state, io::discard_writer& no_body)
{
    const auto& req  = state.req;
    auto&       resp = state.resp;

    // A response to HEAD has the headers a GET would, but no body: what the handler writes as one is dropped.
    resp.version     = response_version(req.version);
    resp.status_code = status::NONE;
    resp.body        = req.method == request_method::HEAD ? static_cast<io::writer*>(&no_body) : &state.writer;
    resp.headers.clear();
    resp.headers.set("Date"sv, current_date());

    // HTTP/1.1 connections persist unless either end says they close, and HTTP/1.0's close unless the client says they
    // persist (RFC 9112, section 9.3): the response says which, if it isn't the default.
    const bool http10 = req.version.minor == 0;
    if (has_close(req.headers) || (http10 && !req.headers.has_token("Connection"sv, "keep-alive"sv)))
        resp.headers.set("Connection"sv, "close"sv);
    else if (http10) resp.headers.set("Connection"sv, "keep-alive"sv);
}

coro::task<> server::end_after_decode_error(detail::connection& conn, connection_state& state, std::error_code err)
{
    if (err == io::status_condition::closed)
    {
        logger->debug("connection closed");
        co_return;
    }

    if (err == std::errc::timed_out)
    {
        logger->debug("timed out waiting for request headers");
        co_return;
    }

    logger->debug("request decoding error: {} '{}'", err.value(), err.message());

    // the rest of the connection can't be trusted to be where the next request starts, so it's closed.
    co_await respond_and_close(conn, state, decode_error_status(err));
}

coro::task<> server::end_after_handler_failure(detail::connection& conn,
                                               connection_state&   state,
                                               response_writer&    rw,
                                               std::exception_ptr  failure)
{
    const auto& req = state.req;

    try
    {
        std::rethrow_exception(failure);
    }
    catch (const std::exception& ex)
    {
        logger->error("handler for {} {} threw: {}", method_string(req.method), req.target.path, ex.what());
    }
    catch (...)
    {
        logger->error("handler for {} {} threw", method_string(req.method), req.target.path);
    }

    // whatever the handler sent of the response can't be finished, so the connection is closed.
    if (rw.status_code() == status::NONE)
    {
        co_await respond_and_close(conn, state, status::INTERNAL_SERVER_ERROR);
        co_return;
    }

    co_await state.writer.flush();
    co_await linger_before_close(conn);
}

coro::task<> server::respond_and_close(detail::connection& conn, connection_state& state, status code)
{
    auto& resp = state.resp;

    resp.version = response_version(state.req.version);

    resp.status_code = status::NONE;
    resp.body        = &state.writer;
    resp.headers.clear();
    resp.headers.set("Connection"sv, "close"sv);
    resp.headers.set("Date"sv, current_date());

    response_writer rw{&state.writer, &resp, http11::response_encode};
    co_await rw.send(code, 0);
    co_await state.writer.flush();
    co_await linger_before_close(conn);
}

}
