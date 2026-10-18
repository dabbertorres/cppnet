#include "http/http2_client.hpp"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "url.hpp"

namespace net::http::detail
{

namespace
{

    // http2_response_body reads a response's body from its stream, which it holds (and its connection): destroying it
    // before the body's end resets the stream.
    class http2_response_body final : public io::reader
    {
    public:
        http2_response_body(std::shared_ptr<http2_client_connection> conn,
                            std::shared_ptr<http2_client_stream>     stream) noexcept
            : conn{std::move(conn)}
            , stream{std::move(stream)}
            , reader{this->stream.get()}
        {}

        http2_response_body(const http2_response_body&)            = delete;
        http2_response_body& operator=(const http2_response_body&) = delete;
        http2_response_body(http2_response_body&&)                 = delete;
        http2_response_body& operator=(http2_response_body&&)      = delete;

        // NOLINTNEXTLINE(bugprone-exception-escape): done() only throws if it can't allocate, when terminating is fine.
        ~http2_response_body() override { conn->done(stream->id); }

        coro::task<io::result> read(std::span<std::byte> data) override { return reader.read(data); }
        using io::reader::read;

        [[nodiscard]] io::handle native_handle() const noexcept override { return conn->native_handle(); }

    private:
        std::shared_ptr<http2_client_connection> conn;
        std::shared_ptr<http2_client_stream>     stream;
        stream_reader                            reader;
    };

    std::string authority_of(const url& uri)
    {
        // an IPv6 address is bracketed, as in a URL.
        std::string out = uri.host.contains(':') ? "[" + uri.host + "]" : uri.host;
        if (!uri.port.empty()) out += ":" + uri.port;
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
    if (ending() || peer_goaway || !session.can_open_stream(reserved)) return false;

    ++reserved;
    return true;
}

bool http2_client_connection::idle()
{
    std::scoped_lock lock{mu};
    return streams.empty();
}

coro::task<std::expected<client_response, std::error_code>> http2_client_connection::send(client_request& request)
{
    std::shared_ptr<http2_client_stream> stream;
    {
        std::scoped_lock lock{mu};
        auto             opened = open(request);
        if (!opened.has_value()) co_return std::unexpected(opened.error());
        stream = std::move(*opened);
    }
    wake_writer();

    // a server may answer before it's read all of the body (e.g. 413 Content Too Large), and reset the stream, failing
    // the rest: the response is still worth having.
    std::optional<std::error_code> body_error;
    if (request.body != nullptr)
    {
        if (auto err = co_await send_body(request, *stream)) body_error = err;
    }

    co_return co_await wait_for_head(std::move(stream), body_error);
}

std::expected<std::shared_ptr<http2_client_stream>, std::error_code>
http2_client_connection::open(const client_request& request)
{
    // the stream reserved for it (if it was), whether or not it can be opened.
    if (reserved > 0) --reserved;

    if (ending() || peer_goaway) return std::unexpected(make_error_code(http2::error_code::refused_stream));

    const auto id = session.open_stream();
    if (!id.has_value()) return std::unexpected(id.error());

    auto stream           = std::make_shared<http2_client_stream>(this, *id, pool);
    stream->read_timeout  = options.io_timeout;
    stream->write_timeout = options.io_timeout;
    stream->authority     = authority_of(request.uri);
    stream->path          = request.uri.origin_form();
    if (stream->path.empty()) stream->path = "/";

    // the pseudo-headers first (RFC 9113, section 8.3.1).
    auto& fields = stream->fields;
    fields.push_back({.name = ":method", .value = method_string(request.method)});
    fields.push_back({.name = ":scheme", .value = is_tls(conn) ? "https" : "http"});
    fields.push_back({.name = ":authority", .value = stream->authority});
    fields.push_back({.name = ":path", .value = stream->path});
    append_fields(request.headers, stream->names, fields, true);

    const bool end = request.body == nullptr;
    if (auto err = session.submit_headers(*id, fields, end))
    {
        session.reset(*id, http2::error_code::internal_error);
        return std::unexpected(err);
    }

    stream->ended = end;
    streams.emplace(*id, stream);
    add_stream(*stream);
    idle_since.reset();
    return stream;
}

coro::task<std::error_code> http2_client_connection::send_body(client_request& request, http2_client_stream& stream)
{
    std::vector<std::byte> buffer(std::size_t{16} * 1'024);
    while (true)
    {
        const auto res = co_await request.body->read(buffer);
        if (res.count > 0)
        {
            const auto written = co_await stream.writer.write(std::span{buffer}.first(res.count));
            if (written.err) co_return written.err;
        }

        if (res.err == io::errc::end_of_stream) break;
        if (res.err)
        {
            // the request can't be finished: the server mustn't take what it has as all of it.
            {
                std::scoped_lock lock{mu};
                if (!stream.failure) stream.failure = res.err;
                session.reset(stream.id, http2::error_code::cancel);
            }
            wake_writer();
            co_return res.err;
        }
    }

    {
        std::scoped_lock lock{mu};
        if (!stream.failure && !stream.ended)
        {
            stream.ended = true;
            if (auto sent = session.submit_data(stream.id, {}, true); !sent.has_value()) stream.failure = sent.error();
        }
    }
    wake_writer();
    co_return std::error_code{};
}

coro::task<std::expected<client_response, std::error_code>>
http2_client_connection::wait_for_head(std::shared_ptr<http2_client_stream> stream,
                                       std::optional<std::error_code>       body_error)
{
    while (true)
    {
        std::uint64_t seen = 0;
        {
            std::scoped_lock lock{mu};
            auto&            s = *stream;

            if (s.head_received)
            {
                s.deadline.reset();

                client_response resp;
                resp.version     = {.major = 2, .minor = 0};
                resp.status_code = s.status_code;
                resp.headers     = std::move(s.headers);
                resp.body        = std::make_unique<http2_response_body>(shared_from_this(), std::move(stream));
                co_return resp;
            }

            if (s.failure)
            {
                // a request the server didn't process may be sent again; one it did, or whose body was cut short,
                // mustn't be.
                auto err = s.failure;
                if (err == http2::error_code::refused_stream && s.processed)
                    err = make_error_code(http2::error_code::cancel);
                if (body_error.has_value() && err != http2::error_code::refused_stream) err = *body_error;
                co_return std::unexpected(err);
            }

            seen = s.changed.value();
            set_deadline(s, options.response_header_timeout);
        }

        (void)co_await stream->changed.wait(seen);
    }
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
            if (streams.empty()) idle_since = io::clock::now();
            maybe_end();
        }
    }

    wake_writer();
    wake_timer();
    if (on.freed) on.freed();
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
    }

    // trailers end the body (and aren't passed on).
    if (end_stream) s.received_end = true;
    s.changed.notify();
}

void http2_client_connection::on_goaway(http2::stream_id /*last*/, std::error_code err, std::string_view /*debug*/)
{
    // no more streams: once those it has are done, so is the connection.
    logger.debug("HTTP/2 server sent GOAWAY: {}", err.message());
    peer_goaway = true;
}

void http2_client_connection::on_ping_ack(std::span<const std::byte, 8> /*opaque*/) {}

std::optional<io::clock::time_point> http2_client_connection::check_connection(io::clock::time_point now)
{
    // a connection without streams is closed once it's been idle for idle_timeout.
    const auto timeout = options.idle_timeout;
    if (timeout <= std::chrono::milliseconds::zero() || !streams.empty()) return std::nullopt;

    if (!idle_since.has_value()) idle_since = now;
    if (now - *idle_since < timeout) return *idle_since + timeout;

    session.goaway(0, http2::error_code::no_error);
    end();
    return std::nullopt;
}

void http2_client_connection::maybe_end()
{
    if (ending()) return;
    if (session.failed() || (peer_goaway && streams.empty())) end();
}

}
