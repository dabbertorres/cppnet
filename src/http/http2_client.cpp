#include "http/http2_client.hpp"

#include <algorithm>
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

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include "coro/notifier.hpp"
#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
#include "http/http2_connection.hpp"
#include "http/http2_scheduler.hpp"
#include "http/http2_session.hpp"
#include "http/origin.hpp"
#include "http/priority.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/structured_fields.hpp"
#include "io/io.hpp"
#include "io/limit_reader.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "tcp.hpp"
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
    // ended (or failed): what's buffered isn't read. It keeps the response's trailers, which the stream's reader hands
    // over as it reaches the body's end (see client_response::trailers()).
    class http2_response_body final
        : public io::reader
        , public body_source
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
            // NOTE: only the body's reads (after this, on its reader's task) look at it, holding the lock.
            this->stream->trailers_to = &kept;

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

        [[nodiscard]] const trailer_fields& trailers() const noexcept override { return kept; }

        bool set_priority(const priority& p) override { return conn->reprioritize(stream->id, p); }

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
        trailer_fields                                            kept;     // the trailers, once it's read to its end
    };

    // keep_trailers keeps a trailers block's fields in the stream (without those that can't be trailers), for its
    // body's reader to hand over at the end (holding the lock).
    void keep_trailers(http2_stream_base& s, std::span<const hpack::field> fields)
    {
        for (const auto& f : fields)
        {
            if (!trailer_allowed(f.name)) continue;
            if (s.received_trailers == nullptr) s.received_trailers = std::make_unique<trailer_fields>();
            s.received_trailers->add(f.name, f.value);
        }
    }

    // request_ends_after is how much DATA ends a request's stream, if that's known (see
    // http2_client_connection::open()). Trailers follow the body, or the head, if there's none (which then doesn't end
    // the stream). Without them, the head ends the stream if there's no body, and the body's last DATA if its length is
    // known; end_request() does otherwise.
    std::optional<std::size_t> request_ends_after(bool has_body, bool trailing, std::optional<std::size_t> length)
    {
        if (trailing) return std::nullopt;
        if (!has_body) return 0;
        return length;
    }

    // body_priority is a request body's priority in the connection's scheduler, which is its request's Priority field
    // (RFC 9218's signal of the response's priority, which a caller marking a request, e.g. u=7 for a background one,
    // means for its upload too), read as the server reads it, over RFC 9218's default (u=3, not incremental: what the
    // field leaves out, or all of it if it doesn't parse); without the field, u=3 incremental, as the server takes a
    // client that never signals, so bodies sent at once without it share the connection a frame each. A field of
    // several lines is joined (as the server does) in a string of its own, as that's rare.
    priority body_priority(const headers& fields)
    {
        using namespace std::string_view_literals;
        const auto lines = fields.get_all("Priority"sv);
        if (!lines.has_value()) return {.urgency = 3, .incremental = true};

        std::string joined;
        return parse_priority(sf::join_lines({lines->begin(), lines->end()}, joined));
    }

    // set_mark sets TCP_NOTSENT_LOWAT on a connection's TCP socket (over TLS, the stream's), if bytes isn't 0,
    // returning whether it's set: its writer then waits for the socket to take more before each round.
    bool set_mark(connection& conn, std::size_t bytes, spdlog::logger& logger)
    {
        if (bytes == 0) return false;

        auto set = socket_of(conn).set_notsent_lowat(bytes);
        if (!set.has_value()) logger.debug("failed to set TCP_NOTSENT_LOWAT: {}", set.error().message());
        return set.has_value();
    }

    // is_continue is whether an informational response's fields say 100 (Continue).
    bool is_continue(std::span<const hpack::field> fields) noexcept
    {
        const auto it = std::ranges::find(fields, std::string_view{":status"}, &hpack::field::name);
        return it != fields.end() && parse_status(it->value) == status::CONTINUE;
    }

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

const https_origin* origin_set::find(std::string_view serialized) const noexcept
{
    const auto it = std::ranges::find(origins, serialized, &https_origin::serialized);
    return it != origins.end() ? &*it : nullptr;
}

void origin_set::add(https_origin o)
{
    if (find(o.serialized) != nullptr) return;
    origins.push_back(std::move(o));
}

std::optional<bool> origin_set::checked_for(std::string_view host) const noexcept
{
    const auto it = std::ranges::find(checked, host, &std::pair<std::string, bool>::first);
    if (it == checked.end()) return std::nullopt;
    return it->second;
}

http2_client_connection::http2_client_connection(io::scheduler*              scheduler,
                                                 std::unique_ptr<connection> conn,
                                                 const http2_client_options& options,
                                                 callbacks                   on,
                                                 std::string                 origin)
    : owned_connection{.owned = std::move(conn), .log = spdlog::default_logger()}
    // TCP_NOTSENT_LOWAT, as the server sets it, now that it's HTTP/2: the kernel then holds little more than what's in
    // flight, and the writer waits for it to take more before framing more, so what's past the scheduler's reordering
    // of request bodies is the mark and a round. (The writable hook is only used while the mark's set.)
    , http2_connection_base{scheduler,
                            *owned,
                            http2::role::client,
                            options.session,
                            *log,
                            set_mark(*owned, options.notsent_lowat, *log),
                            options.writable}
    , options{options}
    , on{std::move(on)}
    , keeps_origins{verifies_peer(*owned)}
    , own_origin{keeps_origins ? std::move(origin) : std::string{}}
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

    const bool trailing   = static_cast<bool>(request.make_trailers);
    const auto ends_after = request_ends_after(has_body, trailing, length);

    // a request expecting 100 (Continue) sends its body once it's had one, or waited long enough (see
    // await_continue()). NOTE: the checks that cost nothing first: a request with a body, but without the expectation,
    // costs the one lookup.
    const bool expecting = has_body && expects_continue(request, options.expect_continue_timeout);

    std::shared_ptr<http2_client_stream> stream;
    {
        std::unique_lock lock{mu};
        auto             opened = open(request, ends_after);
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
        if (expecting) expect_continue(*stream);
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
    bool                           sending = has_body || trailing;
    if (expecting) sending = co_await await_continue(*stream, request, state);
    if (sending)
    {
        const auto sent = co_await send_body(request, *stream, has_body ? length : std::optional<std::size_t>{0});

        // the body couldn't be read, was shorter than its Content-Length, or a trailer couldn't be sent: the request
        // is the caller's mistake (as over HTTP/1.1), whatever the server made of it.
        if (sent.body_failed) co_return std::unexpected(sent.err);
        if (sent.err) body_error = sent.err;
    }

    auto resp = co_await wait_for_head(std::move(stream), body_error, state, request);
    taken     = resp.has_value();
    co_return resp;
}

std::expected<std::shared_ptr<http2_client_stream>, std::error_code>
http2_client_connection::open(const client_request& request, std::optional<std::size_t> ends_after)
{
    // the stream reserved for it (if it was), whether or not it can be opened.
    if (reserved > 0) --reserved;

    if (request.stop.stop_requested()) return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    if (ending() || peer_goaway || draining) return std::unexpected(make_error_code(http2::error_code::refused_stream));

    // the head is made before the stream is opened, as a stream that's opened must be sent.
    auto stream           = std::make_shared<http2_client_stream>(this, 0, pool);
    stream->read_timeout  = options.io_timeout;
    stream->write_timeout = options.io_timeout;
    stream->wants_interim = static_cast<bool>(request.on_informational);
    stream->authority     = authority_of(request);
    stream->path          = request.uri.origin_form();
    if (stream->path.empty()) stream->path = "/";

    // the pseudo-headers first (RFC 9113, section 8.3.1).
    auto& fields = stream->fields;
    fields.push_back({.name = ":method", .value = method_string(request.method)});
    fields.push_back({.name = ":scheme", .value = is_tls(conn) ? "https" : "http"});
    fields.push_back({.name = ":authority", .value = stream->authority});
    fields.push_back({.name = ":path", .value = stream->path});
    if (auto err = append_fields(request.headers, stream->names, fields, field_section::request))
        return std::unexpected(err);

    const auto id = session.open_stream();
    if (!id.has_value()) return std::unexpected(id.error());
    stream->id = *id;
    stream->set_stream(*id);

    const bool end_stream = ends_after == std::size_t{0};

    if (auto err = session.submit_headers(*id, fields, end_stream))
    {
        session.reset(*id, http2::error_code::internal_error);
        return std::unexpected(err);
    }

    stream->ended = end_stream;
    if (!end_stream) stream->data_left = ends_after;
    send_order.update(*stream, body_priority(request.headers));
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
    // no more than its Content-Length: the server would take what's past it as malformed. (Less is checked below.) A
    // length of 0 reads nothing (e.g. a request without a body, sending only trailers).
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

    // NOTE: an exception make_trailers throws comes out of send(): the request can't be finished.
    std::error_code invalid;
    try
    {
        invalid = end_request(request, stream);
    }
    catch (...)
    {
        abandon(stream, std::make_error_code(std::errc::operation_canceled));
        throw;
    }

    // a trailer that can't be sent: the server mustn't take the request as complete.
    if (invalid)
    {
        abandon(stream, invalid);
        co_return body_result{.err = invalid, .body_failed = true};
    }

    wake_writer();
    co_return body_result{};
}

std::error_code http2_client_connection::end_request(const client_request& request, http2_client_stream& stream)
{
    // the trailers are what's known once the body's been read (e.g. a digest of it).
    // NOTE: not in send_body()'s frame, so a request without them doesn't make it larger.
    trailer_fields trailers;
    if (request.make_trailers) request.make_trailers(trailers);

    // NOTE: a body of known length without trailers has ended the stream with its last DATA (see open()).
    std::scoped_lock lock{mu};
    if (stream.failure || stream.ended) return {};

    // they end the stream, if there are any it can send; an empty DATA frame does otherwise. If one can't be sent,
    // nothing is.
    stream.fields.clear();
    if (!trailers.empty())
    {
        if (auto err = append_fields(trailers, stream.names, stream.fields, field_section::trailers)) return err;
    }

    stream.ended = true;
    if (!stream.fields.empty())
    {
        if (auto err = session.submit_headers(stream.id, stream.fields, true)) fail_stream(stream, err);
    }
    else if (auto ended = session.submit_data(stream.id, {}, true); !ended.has_value())
    {
        fail_stream(stream, ended.error());
    }

    return {};
}

coro::task<bool>
http2_client_connection::await_continue(http2_client_stream& stream, const client_request& request, send_state* state)
{
    std::vector<http2_client_stream::interim_head> interims; // taken from the stream, to pass on without the lock
    while (true)
    {
        // NOTE: an exception the callback throws comes out of send(), whose frame then resets the stream (done()).
        for (const auto& head : interims) request.on_informational(head.status_code, head.headers);
        interims.clear();

        std::uint64_t seen = 0;
        {
            std::scoped_lock lock{mu};
            auto&            s = stream;

            // in order: the interim responses before what comes after them.
            if (!s.interims.empty())
            {
                interims.swap(s.interims);
                continue;
            }

            // a final response first, above 299 (Go's rule): the body's not sent, the server having refused the request
            // (e.g. 413 Content Too Large), or the expectation (417, which the request may be sent again for, without
            // it).
            if (s.head_received && static_cast<std::uint16_t>(s.status_code) > 299)
            {
                stop_awaiting(s);
                if (s.status_code == status::EXPECTATION_FAILED && state != nullptr) state->expectation_failed = true;
                co_return false;
            }

            // reset, canceled, or the connection's ended: wait_for_head() says what came of it.
            if (s.failure)
            {
                stop_awaiting(s);
                co_return false;
            }

            // a 100, or none in time. A 2xx first goes on waiting for the timeout (the stream may be full-duplex).
            if (s.continued || s.continue_waited)
            {
                stop_awaiting(s);
                co_return true;
            }

            seen = s.changed.value();
        }

        (void)co_await stream.changed.wait(seen);
    }
}

void http2_client_connection::expect_continue(http2_client_stream& s)
{
    s.awaiting_continue = true;
    s.continue_deadline = io::clock::now() + options.expect_continue_timeout;
    ++awaiting_continue;

    // (the timer finds when it's due once it looks.)
    wake_timer();
}

void http2_client_connection::stop_awaiting(http2_client_stream& s) noexcept
{
    if (!s.awaiting_continue) return;
    s.awaiting_continue = false;
    --awaiting_continue;
}

std::optional<io::clock::time_point> http2_client_connection::check_continues(io::clock::time_point now)
{
    // NOTE: a connection without requests waiting (nearly every one) doesn't look.
    if (awaiting_continue == 0) return std::nullopt;

    // a wait that's timed out sends the body: the stream doesn't fail.
    std::optional<io::clock::time_point> next;
    for (auto& [id, s] : streams)
    {
        if (!s->awaiting_continue) continue;
        if (s->continue_deadline <= now)
        {
            stop_awaiting(*s);
            s->continue_waited = true;
            s->changed.notify();
        }
        else if (!next.has_value() || s->continue_deadline < *next)
        {
            next = s->continue_deadline;
        }
    }
    return next;
}

void http2_client_connection::abandon(http2_client_stream& stream, std::error_code err)
{
    // the request can't be finished: the server mustn't take what it has as all of it.
    {
        std::scoped_lock lock{mu};
        fail_stream(stream, err);
        session.reset(stream.id, http2::error_code::cancel);
    }
    wake_writer();
}

coro::task<std::expected<client_response, std::error_code>>
http2_client_connection::wait_for_head(std::shared_ptr<http2_client_stream> stream,
                                       std::optional<std::error_code>       body_error,
                                       send_state*                          state,
                                       const client_request&                request)
{
    client_response                                resp;
    std::vector<http2_client_stream::interim_head> interims; // taken from the stream, to pass on without the lock
    while (true)
    {
        // the informational responses that came first, in order, before the final head or the stream's failure.
        // NOTE: an exception the callback throws comes out of send(), whose frame then resets the stream (done()).
        for (const auto& head : interims) request.on_informational(head.status_code, head.headers);
        interims.clear();

        std::uint64_t seen = 0;
        {
            std::scoped_lock lock{mu};
            auto&            s = *stream;

            if (!s.interims.empty())
            {
                interims.swap(s.interims);
                continue;
            }

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
    resp.body = std::make_unique<http2_response_body>(shared_from_this(), std::move(stream), request.stop);
    co_return resp;
}

void http2_client_connection::done(http2::stream_id id)
{
    {
        std::scoped_lock lock{mu};
        const auto       it = streams.find(id);
        if (it != streams.end())
        {
            // a response not read to its end: the server needn't send the rest. A request not sent to its end (its
            // body not sent, as a final response came first, refusing it) is reset too, so the stream closes.
            auto& s = *it->second;
            if ((!s.received_end || !s.ended) && !s.failure) session.reset(id, http2::error_code::cancel);
            stop_awaiting(s);

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

bool http2_client_connection::reprioritize(http2::stream_id id, priority p)
{
    {
        std::scoped_lock lock{mu};
        const auto       it = streams.find(id);
        if (it == streams.end()) return false;

        // a response that's failed (e.g. the connection's ended, which the session needn't know) has nothing left to
        // reprioritize; nor has one that's ended (all of it's come, though not all may have been read), which the
        // session refuses.
        auto& s = *it->second;
        if (s.failure) return false;

        const auto sent = session.submit_priority_update(id, p);
        if (!sent.has_value() || !*sent) return false;

        // the request's body too, in this side's scheduler: from its next frame, if it's waiting (whose urgencies past
        // 7 are 7's, as the session sends them).
        send_order.update(s, p);
    }

    // (only if there's output: an update that changes nothing sends nothing.)
    wake_writer();
    return true;
}

void http2_client_connection::cancel(http2::stream_id id)
{
    bool freed = false;
    {
        std::scoped_lock lock{mu};
        const auto       it = streams.find(id);
        if (it == streams.end()) return;

        // its waits (for the response's head, its body, or its turn to send the request's body, which returns what
        // was framed of it) see it fail; the server, its reset (unless it's done with the stream already).
        auto& s = *it->second;
        fail_stream(s, std::make_error_code(std::errc::operation_canceled));
        session.reset(id, http2::error_code::cancel);
        s.read_deadline.reset();
        s.write_deadline.reset();
        s.changed.notify();

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

    if (kind == http2::block_kind::informational)
    {
        on_interim(s, fields);
        return;
    }

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

    // trailers end the body: kept (without those that can't be trailers) for the body's reader to hand over once it
    // reaches the end, so whoever reads them does once the body's been read, and they don't change meanwhile.
    if (kind == http2::block_kind::trailers) keep_trailers(s, fields);

    if (end_stream)
    {
        s.received_end = true;
        s.read_deadline.reset();
    }
    s.changed.notify();
}

void http2_client_connection::on_interim(http2_client_stream& s, std::span<const hpack::field> fields)
{
    // informational responses (e.g. 103 Early Hints) are kept for send()'s task to pass on, if the request wants them.
    // They're limited altogether, as their number isn't (Go's limit, when it keeps them): past it, the request fails.
    // NOTE: neither is progress towards the final head, so the stream's read deadline stays.
    for (const auto& f : fields) s.interim_size += hpack::field_size(f.name, f.value);
    if (s.interim_size > options.session.local.max_header_list_size)
    {
        logger.debug("HTTP/2 stream {}: its informational responses are too large", s.id);
        fail_stream(s, std::make_error_code(std::errc::value_too_large));
        session.reset(s.id, http2::error_code::protocol_error);
        s.read_deadline.reset();
        s.write_deadline.reset();
        s.changed.notify();
        return;
    }

    // a 100 (Continue) is what a request expecting it waits for, whether or not it wants interim responses.
    if (!s.wants_interim)
    {
        if (s.awaiting_continue && is_continue(fields))
        {
            s.continued = true;
            s.changed.notify();
        }
        return;
    }

    auto& head = s.interims.emplace_back();
    for (const auto& f : fields)
    {
        if (f.name == ":status") head.status_code = parse_status(f.value);
        else if (!f.name.starts_with(':')) head.headers.add(f.name, f.value);
    }
    if (head.status_code == status::CONTINUE) s.continued = true;
    s.changed.notify();
}

// (a client's session never reports it: a response's head over the limit resets its stream.)
void http2_client_connection::on_header_list_too_large(http2::stream_id /*stream*/, bool /*end_stream*/) {}

void http2_client_connection::on_goaway(http2::stream_id /*last*/, std::error_code err, std::string_view /*debug*/)
{
    // no more streams: once those it has are done, so is the connection.
    logger.debug("HTTP/2 server sent GOAWAY: {}", err.message());
    peer_goaway = true;
}

void http2_client_connection::on_ping_ack(std::span<const std::byte, 8> /*opaque*/) {}

// (a client's session never reports it: a PRIORITY_UPDATE from the server ends the connection.)
void http2_client_connection::on_priority_update(http2::stream_id /*stream*/, priority /*p*/) {}

void http2_client_connection::on_origin(std::span<const std::string_view> entries)
{
    logger.debug("HTTP/2 server sent ORIGIN, with {} entries", entries.size());
    if (!keeps_origins) return;

    // the first initializes the set, with the connection's own origin (RFC 8336, section 2.3).
    if (known_origins == nullptr)
    {
        known_origins = std::make_unique<origin_set>();
        if (auto own = parse_origin(own_origin); own.has_value()) known_origins->add(std::move(*own));
        tell_origins = true;
    }

    // an entry that isn't an https origin is ignored; once the set's full, so is the rest (not parsed). NOTE: the one
    // place the limit's kept (the own origin, added first, always fits).
    for (const auto entry : entries)
    {
        if (known_origins->origins.size() >= origin_set::max_origins) break;
        if (auto o = parse_origin(entry); o.has_value()) known_origins->add(std::move(*o));
    }
}

bool http2_client_connection::authoritative_for(std::string_view origin)
{
    auto wanted = parse_origin(origin);
    if (!wanted.has_value()) return false;

    {
        std::scoped_lock lock{mu};
        if (known_origins == nullptr || known_origins->find(wanted->serialized) == nullptr) return false;
        if (const auto known = known_origins->checked_for(wanted->host); known.has_value()) return *known;
    }

    // NOTE: without the lock, as the connection goes on: its reads and writes don't wait for the signatures. Two
    //       callers at once may both check a host, and find the same.
    const bool valid = !verify_for(*owned, wanted->host);

    std::scoped_lock lock{mu};
    if (!known_origins->checked_for(wanted->host).has_value()) known_origins->checked.emplace_back(wanted->host, valid);

    // (it may have been removed meanwhile.)
    return valid && known_origins->find(wanted->serialized) != nullptr;
}

void http2_client_connection::remove_origin(std::string_view origin)
{
    const auto unwanted = parse_origin(origin);
    if (!unwanted.has_value()) return;

    std::scoped_lock lock{mu};
    if (known_origins == nullptr) return;
    std::erase_if(known_origins->origins, [&](const https_origin& o) { return o.serialized == unwanted->serialized; });
}

void http2_client_connection::after_receive()
{
    // a stream the session's done with (its response ended, or it was reset) is free for another request, though its
    // response may not have been read yet: a request waiting for one (strict_max_concurrent_streams) is told now.
    bool freed = false;
    bool known = false;
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

        // the server's first ORIGIN has initialized the Origin Set.
        known = std::exchange(tell_origins, false);
    }

    if (freed && on.freed) on.freed();
    if (known && on.origins_known) on.origins_known(*this);
}

std::optional<io::clock::time_point> http2_client_connection::check_connection(io::clock::time_point now)
{
    // (a connection with none waits for 100 (Continue).)
    if (!idle_now()) return check_continues(now);

    // a connection without streams (nor any reserved) is closed once it's been idle for idle_timeout (at once, if
    // that's 0).

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
