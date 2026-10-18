#include "http/http2_client.hpp"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "coro/notifier.hpp"
#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/hpack.hpp"
#include "http/http.hpp"
#include "http/http2.hpp"
#include "http/http2_connection.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/io.hpp"
#include "io/limit_reader.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "url.hpp"
#include "util/defer.hpp"
#include "util/string_util.hpp"

namespace net::http::detail
{

namespace
{

    // http2_response_body reads a response's body from its stream, which it holds (and its connection): destroying it
    // before the body's end resets the stream. Given a stop token that can be stopped, it cancels the stream when stop
    // is requested, and its reads fail with std::errc::operation_canceled once it is, unless the body had already
    // ended (or failed): what's buffered isn't read.
    class http2_response_body final : public io::reader
    {
    public:
        // NOTE: not holding the connection's lock: the stop callback is registered here, and runs at once (taking the
        //       lock) if stop was requested already.
        http2_response_body(std::shared_ptr<http2_client_connection> conn,
                            std::shared_ptr<http2_client_stream>     stream,
                            const std::stop_token&                   stop)
            : conn{std::move(conn)}
            , stream{std::move(stream)}
            , reader{this->stream.get()}
        {
            if (!stop.stop_possible()) return;

            this->stop = stop;
            on_stop.emplace(stop, http2_client_connection::canceler{.conn = this->conn.get(), .id = this->stream->id});
        }

        http2_response_body(const http2_response_body&)            = delete;
        http2_response_body& operator=(const http2_response_body&) = delete;
        http2_response_body(http2_response_body&&)                 = delete;
        http2_response_body& operator=(http2_response_body&&)      = delete;

        // NOLINTNEXTLINE(bugprone-exception-escape): done() only throws if it can't allocate, when terminating is fine.
        ~http2_response_body() override
        {
            // NOTE: first, and not holding the lock, which a callback running meanwhile (that this waits for) takes.
            on_stop.reset();
            conn->done(stream->id);
        }

        coro::task<io::result> read(std::span<std::byte> data) override
        {
            // NOTE: without a token, the stream's reader's task as it is (it often needn't wait, nor a coroutine).
            if (!stop.stop_possible()) return reader.read(data);
            return read_stoppable(data);
        }
        using io::reader::read;

        [[nodiscard]] io::handle native_handle() const noexcept override { return conn->native_handle(); }

    private:
        coro::task<io::result> read_stoppable(std::span<std::byte> data)
        {
            if (finished) co_return io::result{.err = finished};

            // NOTE: checked first, as what's buffered would be read without waiting (so without seeing the stream
            //       fail).
            const auto canceled = std::make_error_code(std::errc::operation_canceled);
            if (stop.stop_requested())
            {
                finished = canceled;
                co_return io::result{.err = finished};
            }

            auto res = co_await reader.read(data);
            if (res.err)
            {
                if (res.err != io::errc::end_of_stream && stop.stop_requested()) res.err = canceled;
                finished = res.err;
            }
            co_return res;
        }

        std::shared_ptr<http2_client_connection> conn;
        std::shared_ptr<http2_client_stream>     stream;
        stream_reader                            reader;

        std::stop_token                                           stop;
        std::optional<http2_client_connection::stop_registration> on_stop;
        std::error_code                                           finished; // what ended it, given a token
    };

    // authority_of is a request's :authority: the caller's Host, if it has one (RFC 9113, section 8.3.1, as it isn't
    // sent as a field; without whitespace around it, as other fields); otherwise the URI's host and port, without the
    // scheme's default port, as HTTP/1.1's Host.
    std::string authority_of(const client_request& request)
    {
        using namespace std::string_view_literals;

        if (const auto host = request.headers.get("Host"sv); host.has_value()) return std::string{trim_value(*host)};

        const auto& uri = request.uri;

        // an IPv6 address is bracketed, as in a URL.
        std::string out = uri.host.contains(':') ? "[" + uri.host + "]" : uri.host;

        const bool https        = util::equal_ignore_case(std::string_view{uri.scheme}, "https"sv);
        const bool default_port = uri.port == (https ? "443"sv : "80"sv);
        if (!uri.port.empty() && !default_port) out += ":" + uri.port;
        return out;
    }

}

http2_client_connection::http2_client_connection(io::scheduler*              scheduler,
                                                 std::unique_ptr<connection> conn,
                                                 const http2_client_options& options,
                                                 callbacks                   on)
    : owned_connection{.owned = std::move(conn), .log = spdlog::default_logger()}
    , http2_connection_base{scheduler, *owned, http2::role::client, options.session, *log}
    , options{options}
    , on{std::move(on)}
{}

bool http2_client_connection::start() { return scheduler->spawn(run_owned(shared_from_this())); }

coro::task<> http2_client_connection::run_owned(std::shared_ptr<http2_client_connection> self)
{
    co_await self->run({});

    // over TLS, after close_notify.
    co_await detail::close(*self->owned);
    if (self->on.closed) self->on.closed(*self);
}

bool http2_client_connection::reserve()
{
    std::scoped_lock lock{mu};
    if (ending() || peer_goaway || draining || !session.can_open_stream(reserved)) return false;

    ++reserved;
    idle_from.reset();
    return true;
}

bool http2_client_connection::idle()
{
    std::scoped_lock lock{mu};
    return idle_now();
}

std::optional<io::clock::time_point> http2_client_connection::idle_since()
{
    std::scoped_lock lock{mu};
    if (!idle_now()) return std::nullopt;
    return idle_from.value_or(io::clock::now());
}

bool http2_client_connection::accepting()
{
    std::scoped_lock lock{mu};
    return !ending() && !peer_goaway && !draining && !session.failed();
}

coro::task<std::expected<client_response, std::error_code>> http2_client_connection::send(client_request& request,
                                                                                          send_state*     state)
{
    // exactly Content-Length bytes of the body are sent, if it's set (as HTTP/1.1's): none, if it's 0.
    const auto length   = request.body != nullptr ? request.headers.get_content_length() : std::nullopt;
    const bool has_body = request.body != nullptr && (!length.has_value() || *length > 0);

    std::shared_ptr<http2_client_stream> stream;
    {
        std::unique_lock lock{mu};
        auto             opened = open(request, !has_body);
        if (!opened.has_value())
        {
            // nothing was sent, nor the body read: a refused request may be sent again (on another connection),
            // whatever it is.
            if (state != nullptr) state->unsent = opened.error() == http2::error_code::refused_stream;

            // the stream that was reserved for it is free (for another request, or to be idle).
            if (idle_now())
            {
                idle_from = io::clock::now();
                wake_timer();
            }
            maybe_end();
            lock.unlock();

            wake_writer();
            if (on.freed) on.freed();
            co_return std::unexpected(opened.error());
        }
        stream = std::move(*opened);
    }
    wake_writer();

    // the stream is done with unless the response's body takes it: whether the request fails (refused, reset, timed
    // out, canceled), or this coroutine is destroyed while it waits.
    const auto id    = stream->id;
    bool       taken = false;
    // NOTE: done() checks on.freed before calling it, and only throws if it can't allocate, when terminating is fine.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    util::defer release{[this, id, &taken] noexcept
                        {
                            if (!taken) done(id);
                        }};

    // canceled from now until the response's head (its body then takes over): stop requested since open() looked
    // resets the stream at once. Destroyed before done(), not holding the lock.
    std::optional<stop_registration> on_stop;
    if (request.stop.stop_possible()) on_stop.emplace(request.stop, canceler{.conn = this, .id = id});

    // a server may answer before it's read all of the body (e.g. 413 Content Too Large), and reset the stream, failing
    // the rest: the response is still worth having.
    std::optional<std::error_code> body_error;
    if (has_body)
    {
        const auto sent = co_await send_body(request, *stream, length);

        // the body couldn't be read, or was shorter than its Content-Length: the request is the caller's mistake (as
        // over HTTP/1.1), whatever the server made of it.
        if (sent.body_failed) co_return std::unexpected(sent.err);
        if (sent.err) body_error = sent.err;
    }

    auto resp = co_await wait_for_head(std::move(stream), body_error, state, request.stop);
    taken     = resp.has_value();
    co_return resp;
}

std::expected<std::shared_ptr<http2_client_stream>, std::error_code>
http2_client_connection::open(const client_request& request, bool end_stream)
{
    // the stream reserved for it (if it was), whether or not it can be opened.
    if (reserved > 0) --reserved;

    if (request.stop.stop_requested()) return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    if (ending() || peer_goaway || draining) return std::unexpected(make_error_code(http2::error_code::refused_stream));

    // the head is made before the stream is opened, as a stream that's opened must be sent.
    auto stream           = std::make_shared<http2_client_stream>(this, 0, pool);
    stream->read_timeout  = options.io_timeout;
    stream->write_timeout = options.io_timeout;
    stream->authority     = authority_of(request);
    stream->path          = request.uri.origin_form();
    if (stream->path.empty()) stream->path = "/";

    // the pseudo-headers first (RFC 9113, section 8.3.1).
    auto& fields = stream->fields;
    fields.push_back({.name = ":method", .value = method_string(request.method)});
    fields.push_back({.name = ":scheme", .value = is_tls(conn) ? "https" : "http"});
    fields.push_back({.name = ":authority", .value = stream->authority});
    fields.push_back({.name = ":path", .value = stream->path});
    if (auto err = append_fields(request.headers, stream->names, fields, true)) return std::unexpected(err);

    const auto id = session.open_stream();
    if (!id.has_value()) return std::unexpected(id.error());
    stream->id = *id;

    if (auto err = session.submit_headers(*id, fields, end_stream))
    {
        session.reset(*id, http2::error_code::internal_error);
        return std::unexpected(err);
    }

    stream->ended = end_stream;
    streams.emplace(*id, stream);
    add_stream(*stream);
    idle_from.reset();

    // NOTE: counted rather than looked up, so a stream a receive frees meanwhile is still noticed (see after_receive).
    ++open_seen;
    return stream;
}

coro::task<http2_client_connection::body_result> http2_client_connection::send_body(client_request&            request,
                                                                                    http2_client_stream&       stream,
                                                                                    std::optional<std::size_t> length)
{
    // no more than its Content-Length: the server would take what's past it as malformed. (Less is checked below.)
    io::limit_reader body{request.body.get(),
                          length.value_or(std::numeric_limits<std::size_t>::max()),
                          io::limit_reader::mode::up_to};

    std::vector<std::byte> buffer(std::size_t{16} * 1'024);
    std::size_t            sent = 0;
    while (!length.has_value() || sent < *length)
    {
        const auto res = co_await body.read(buffer);
        if (res.count > 0)
        {
            const auto written = co_await stream.writer.write(std::span{buffer}.first(res.count));
            if (written.err) co_return body_result{.err = written.err};
            sent += res.count;
        }

        auto err = res.err;
        if (err == io::errc::end_of_stream)
        {
            // a body shorter than its Content-Length is the caller's mistake.
            if (!length.has_value() || sent == *length) break;
            err = std::make_error_code(std::errc::invalid_argument);
        }

        // a reader that returns nothing, without failing, would be read forever.
        if (!err && res.count == 0) err = make_error_code(io::errc::no_progress);

        if (err)
        {
            abandon(stream, err);
            co_return body_result{.err = err, .body_failed = true};
        }
    }

    {
        std::scoped_lock lock{mu};
        if (!stream.failure && !stream.ended)
        {
            stream.ended = true;
            if (auto ended = session.submit_data(stream.id, {}, true); !ended.has_value())
                stream.failure = ended.error();
        }
    }
    wake_writer();
    co_return body_result{};
}

void http2_client_connection::abandon(http2_client_stream& stream, std::error_code err)
{
    // the request can't be finished: the server mustn't take what it has as all of it.
    {
        std::scoped_lock lock{mu};
        if (!stream.failure) stream.failure = err;
        session.reset(stream.id, http2::error_code::cancel);
    }
    wake_writer();
}

coro::task<std::expected<client_response, std::error_code>>
http2_client_connection::wait_for_head(std::shared_ptr<http2_client_stream> stream,
                                       std::optional<std::error_code>       body_error,
                                       send_state*                          state,
                                       const std::stop_token&               stop)
{
    client_response resp;
    while (true)
    {
        std::uint64_t seen = 0;
        {
            std::scoped_lock lock{mu};
            auto&            s = *stream;

            if (s.head_received)
            {
                resp.version     = {.major = 2, .minor = 0};
                resp.status_code = s.status_code;
                resp.headers     = std::move(s.headers);
                break;
            }

            if (s.failure)
            {
                // a request the server didn't process may be sent again; one it did, or whose body was cut short,
                // mustn't be.
                auto err = s.failure;
                if (err == http2::error_code::refused_stream && s.processed)
                    err = make_error_code(http2::error_code::cancel);
                if (body_error.has_value() && err != http2::error_code::refused_stream) err = *body_error;

                // the connection ended (not after the server's GOAWAY, which says what it processed) before any
                // response: the request may or may not have been processed.
                if (state != nullptr)
                    state->dropped = !s.processed && !peer_goaway && err == std::errc::connection_aborted;

                co_return std::unexpected(err);
            }

            seen = s.changed.value();
            arm_deadline(s.read_deadline, options.response_header_timeout);
        }

        (void)co_await stream->changed.wait(seen);
    }

    // NOTE: made without the lock, as it registers stop's callback, which takes it.
    resp.body = std::make_unique<http2_response_body>(shared_from_this(), std::move(stream), stop);
    co_return resp;
}

void http2_client_connection::done(http2::stream_id id)
{
    {
        std::scoped_lock lock{mu};
        const auto       it = streams.find(id);
        if (it != streams.end())
        {
            // a response not read to its end: the server needn't send the rest.
            const auto& s = *it->second;
            if (!s.received_end && !s.failure) session.reset(id, http2::error_code::cancel);

            remove_stream(id);
            streams.erase(it);
            if (idle_now()) idle_from = io::clock::now();
            open_seen = session.open_streams();
            maybe_end();
        }
    }

    wake_writer();
    wake_timer();
    if (on.freed) on.freed();
}

void http2_client_connection::cancel(http2::stream_id id)
{
    bool freed = false;
    {
        std::scoped_lock lock{mu};
        const auto       it = streams.find(id);
        if (it == streams.end()) return;

        // its waits (for the response's head, its body, or a window to send the request's body) see it fail; the
        // server, its reset (unless it's done with the stream already).
        auto& s = *it->second;
        fail_stream(s, std::make_error_code(std::errc::operation_canceled));
        session.reset(id, http2::error_code::cancel);
        s.read_deadline.reset();
        s.write_deadline.reset();
        s.changed.notify();

        // a writer may be waiting for the connection's writer to catch up, rather than for the stream.
        output_drained().notify();

        // the session's done with the stream, so a request waiting for one may have it, though the request's response
        // hasn't been dropped yet (as after_receive() tells).
        const auto open_now = session.open_streams();
        freed               = open_now < open_seen;
        open_seen           = open_now;
    }

    wake_writer();
    if (freed && on.freed) on.freed();
}

void http2_client_connection::close()
{
    {
        std::scoped_lock lock{mu};
        if (ending()) return;
        session.goaway(0, http2::error_code::no_error);
        end();
    }
    wake_writer();
}

void http2_client_connection::drain()
{
    {
        std::scoped_lock lock{mu};
        if (ending() || draining) return;

        // no more streams (the session opens none after its own GOAWAY), and the connection ends with its last.
        draining = true;
        session.goaway(0, http2::error_code::no_error);
        maybe_end();
    }
    wake_writer();
}

void http2_client_connection::on_headers(http2::stream_id              id,
                                         http2::block_kind             kind,
                                         std::span<const hpack::field> fields,
                                         bool                          end_stream)
{
    const auto it = streams.find(id);
    if (it == streams.end()) return;

    auto& s     = *it->second;
    s.processed = true;

    // informational responses (e.g. 103 Early Hints) aren't passed on.
    if (kind == http2::block_kind::informational) return;

    if (kind == http2::block_kind::head)
    {
        for (const auto& f : fields)
        {
            if (f.name == ":status") s.status_code = parse_status(f.value);
            else if (!f.name.starts_with(':')) s.headers.add(f.name, f.value);
        }
        s.head_received = true;
        s.read_deadline.reset();
    }

    // trailers end the body (and aren't passed on).
    if (end_stream)
    {
        s.received_end = true;
        s.read_deadline.reset();
    }
    s.changed.notify();
}

void http2_client_connection::on_goaway(http2::stream_id /*last*/, std::error_code err, std::string_view /*debug*/)
{
    // no more streams: once those it has are done, so is the connection.
    logger.debug("HTTP/2 server sent GOAWAY: {}", err.message());
    peer_goaway = true;
}

void http2_client_connection::on_ping_ack(std::span<const std::byte, 8> /*opaque*/) {}

void http2_client_connection::after_receive()
{
    // a stream the session's done with (its response ended, or it was reset) is free for another request, though its
    // response may not have been read yet: a request waiting for one (strict_max_concurrent_streams) is told now.
    bool freed = false;
    {
        std::scoped_lock lock{mu};
        const auto       open_now = session.open_streams();
        freed                     = open_now < open_seen;
        open_seen                 = open_now;

        // after the server's GOAWAY, a request waiting for one of its streams needn't wait any more.
        if (peer_goaway && !goaway_told)
        {
            goaway_told = true;
            freed       = true;
        }
    }

    if (freed && on.freed) on.freed();
}

std::optional<io::clock::time_point> http2_client_connection::check_connection(io::clock::time_point now)
{
    // a connection without streams (nor any reserved) is closed once it's been idle for idle_timeout (at once, if
    // that's 0).
    if (!idle_now()) return std::nullopt;

    if (!idle_from.has_value()) idle_from = now;
    const auto timeout = options.idle_timeout;
    if (timeout > std::chrono::milliseconds::zero() && now - *idle_from < timeout) return *idle_from + timeout;

    session.goaway(0, http2::error_code::no_error);
    end();
    return std::nullopt;
}

void http2_client_connection::maybe_end()
{
    if (ending()) return;
    if (session.failed())
    {
        end();
        return;
    }

    // done with its last stream: after either side's GOAWAY, or if no connection's to be kept idle.
    if (!idle_now()) return;
    if (draining || peer_goaway)
    {
        end();
    }
    else if (options.idle_timeout <= std::chrono::milliseconds::zero())
    {
        session.goaway(0, http2::error_code::no_error);
        end();
    }
}

}
