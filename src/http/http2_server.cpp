#include "http/http2_server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <deque>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/common.h>
#include <spdlog/logger.h>

#include "coro/async_scope.hpp"
#include "coro/notifier.hpp"
#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http.hpp"
#include "http/http2.hpp"
#include "http/http2_connection.hpp"
#include "http/http2_session.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/discard_writer.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "tls/certificate.hpp"

namespace net::http::detail
{

namespace
{

    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    // How long close() waits for its PING's acknowledgement before sending its final GOAWAY.
    constexpr auto drain_wait = 1s;

    // The PING close() sends, to know when the client has seen its first GOAWAY.
    constexpr std::array<std::byte, 8> drain_ping{std::byte{'d'},
                                                  std::byte{'r'},
                                                  std::byte{'a'},
                                                  std::byte{'i'},
                                                  std::byte{'n'},
                                                  std::byte{'i'},
                                                  std::byte{'n'},
                                                  std::byte{'g'}};

    std::string_view current_date()
    {
        thread_local std::chrono::sys_seconds formatted_at{};
        thread_local std::string              formatted;

        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        if (now != formatted_at)
        {
            formatted    = format_date(now);
            formatted_at = now;
        }
        return formatted;
    }

}

// http2_stream is a request a connection serves: the request, the response its handler makes, and its stream.
class http2_stream final : public http2_stream_base
{
public:
    using http2_stream_base::http2_stream_base;

    server_request  req;
    server_response resp;

    bool head_request = false;

    // the request expects 100 Continue before its client sends the body, which is to come (see run_stream()).
    bool expects_continue = false;

    // req.body, as start_stream() made it.
    stream_reader* body = nullptr;

    io::discard_writer discard;

    // for sending heads, reused.
    std::string               status_text;
    std::vector<std::string>  names;
    std::vector<hpack::field> fields;
};

namespace
{

    // http2_server_connection is an HTTP/2 connection the server serves.
    class http2_server_connection final : public http2_connection_base
    {
    public:
        http2_server_connection(io::scheduler*         scheduler,
                                connection&            conn,
                                const http2_options&   options,
                                const request_handler& handler,
                                spdlog::logger&        logger)
            : http2_connection_base{scheduler, conn, http2::role::server, options.session, logger}
            , options{options}
            , handler{handler}
            , peer_certs{peer_certificates(conn)}
        {}

        coro::task<> serve(std::span<const std::byte> already_read, std::stop_token stop, std::stop_token abort);

    protected:
        // the session's events, and the base's hooks: protected, as they're declared.
        void on_headers(http2::stream_id              id,
                        http2::block_kind             kind,
                        std::span<const hpack::field> fields,
                        bool                          end_stream) override;
        void on_goaway(http2::stream_id last, std::error_code err, std::string_view debug) override;
        void on_ping_ack(std::span<const std::byte, 8> opaque) override;

        void                                 after_receive() override;
        coro::task<>                         finish_streams() override;
        std::optional<io::clock::time_point> check_connection(io::clock::time_point now) override;
        void                                 maybe_end() override;

    private:
        // start_stream makes a stream for a request's head, to start its handler, or answers it (e.g. a method that
        // isn't implemented), returning nullptr then.
        std::unique_ptr<http2_stream>
        start_stream(http2::stream_id id, std::span<const hpack::field> fields, bool end_stream);

        // refuse answers a request with just a status, ending the stream.
        void refuse(http2::stream_id id, status code, bool request_ended);

        // schedule starts a stream's handler, or queues it if max_concurrent_streams handlers are running (holding
        // the lock), or ends the connection if too many are queued (removing the stream); promote starts queued
        // handlers as running ones finish (skipping streams reset meanwhile). A queued stream's handler gets its token
        // when it starts: one stopped meanwhile (e.g. by abort) is stopped from the start.
        void schedule(http2_stream& stream);
        void promote();

        // start_handlers spawns the handlers that schedule() and promote() started (without the lock).
        void start_handlers();

        coro::task<> run_stream(http2_stream* stream);

        // stop_handlers is abort's callback: it stops every stream's token (and those of streams started after).
        void stop_handlers();

        // respond_after sends what's left of a response once its handler has returned (or thrown, with failure),
        // returning whether the response is complete; if it's not, the stream's to be reset rather than finished.
        coro::task<bool> respond_after(http2_stream& stream, response_writer& rw, const std::exception_ptr& failure);

        // begin_drain starts a graceful shutdown: GOAWAY, then once the client has seen it (a PING), the final
        // GOAWAY.
        void begin_drain();
        void final_goaway();

        const http2_options&   options;
        const request_handler& handler;
        std::stop_token        abort_token; // the server's abort(), for each request (set by serve(), before streams)

        // the client's certificates, for each request (made once, if it sent any).
        std::shared_ptr<const std::vector<tls::certificate>> peer_certs;

        // Guarded by mu.
        std::unordered_map<http2::stream_id, std::unique_ptr<http2_stream>> streams;
        std::vector<http2_stream*>                                          started; // to start once unlocked
        std::deque<http2_stream*>                                           queued;  // to start once others finish
        std::size_t                                                         running      = 0; // handlers started
        http2::stream_id                                                    last_started = 0; // the highest started
        bool                                                                draining     = false;
        bool                                                                final_sent   = false; // the final GOAWAY
        bool                                                                peer_goaway  = false;
        bool                                                                aborted      = false; // stop_handlers()
        std::optional<io::clock::time_point>                                drain_deadline;
        std::optional<io::clock::time_point>                                idle_since;
        io::clock::time_point                                               started_at = io::clock::now();

        coro::async_scope handlers; // the streams' handlers
    };

    coro::task<std::expected<io::writer*, std::error_code>>
    http2_send_head(http2_stream& stream, const server_response& resp, bool end_stream)
    {
        auto& c = *stream.conn;
        {
            std::scoped_lock lock{c.mu};
            if (stream.failure)
                return coro::ready_task<std::expected<io::writer*, std::error_code>>(std::unexpected(stream.failure));

            // a response to HEAD has no body (nor trailers), whatever its head says; one without a body is followed
            // by its trailers, if it has any (or declares them), which end the stream instead.
            const bool trailing = !resp.trailers.empty() || resp.headers.get_all("Trailer"sv).has_value();
            const bool end =
                (end_stream && !trailing) || (stream.head_request && !status_is_information(resp.status_code));

            stream.status_text = std::to_string(static_cast<int>(resp.status_code));
            stream.fields.clear();
            stream.fields.push_back({.name = ":status", .value = stream.status_text});
            if (auto err = append_fields(resp.headers, stream.names, stream.fields))
                return coro::ready_task<std::expected<io::writer*, std::error_code>>(std::unexpected(err));

            if (auto err = c.session.submit_headers(stream.id, stream.fields, end))
                return coro::ready_task<std::expected<io::writer*, std::error_code>>(std::unexpected(err));

            if (end) stream.ended = true;
        }

        c.wake_writer();
        io::writer* body = stream.ended ? static_cast<io::writer*>(&stream.discard) : &stream.writer;
        return coro::ready_task<std::expected<io::writer*, std::error_code>>(body);
    }

    coro::task<io::result> http2_finish(http2_stream& stream)
    {
        auto& c = *stream.conn;
        {
            std::scoped_lock lock{c.mu};
            if (stream.failure) return coro::ready_task<io::result>(io::result{.err = stream.failure});

            if (!stream.ended)
            {
                // the trailers end the stream, if there are any it can send; an empty DATA frame does otherwise. If
                // one can't be sent, nothing is, and the stream's left to be reset.
                stream.fields.clear();
                if (auto err =
                        append_fields(stream.resp.trailers, stream.names, stream.fields, field_section::trailers))
                    return coro::ready_task<io::result>(io::result{.err = err});

                stream.ended = true;
                if (!stream.fields.empty())
                {
                    if (auto err = c.session.submit_headers(stream.id, stream.fields, true))
                        return coro::ready_task<io::result>(io::result{.err = err});
                }
                else if (auto sent = c.session.submit_data(stream.id, {}, true); !sent.has_value())
                {
                    return coro::ready_task<io::result>(io::result{.err = sent.error()});
                }
            }
        }

        c.wake_writer();
        return coro::ready_task<io::result>();
    }

}

const http2_stream_ops http2_ops{.send_head = http2_send_head, .finish = http2_finish};

namespace
{

    coro::task<>
    http2_server_connection::serve(std::span<const std::byte> already_read, std::stop_token stop, std::stop_token abort)
    {
        // close() (stop) ends the connection gracefully.
        std::stop_callback on_stop{stop,
                                   [this] noexcept
                                   {
                                       {
                                           std::scoped_lock lock{mu};
                                           begin_drain();
                                       }
                                       wake_writer();
                                   }};

        // abort() stops the handlers' tokens too (and runs close()'s callback first).
        abort_token = abort;
        std::stop_callback on_abort{abort, [this] noexcept { stop_handlers(); }};

        co_await run(already_read);
    }

    void http2_server_connection::stop_handlers()
    {
        std::vector<std::stop_source> sources;
        {
            std::scoped_lock lock{mu};
            aborted = true;

            // NOTE: those queued too, whose handlers then start stopped.
            for (const auto& [_, s] : streams)
            {
                if (!s->stop.stop_requested()) sources.push_back(s->stop);
            }
        }

        // NOTE: without the lock (see fail_stream()).
        stop_all(sources);
    }

    void http2_server_connection::after_receive() { start_handlers(); }

    void http2_server_connection::start_handlers()
    {
        std::vector<http2_stream*> to_start;
        {
            std::scoped_lock lock{mu};
            to_start.swap(started);
        }

        bool refused = false;
        for (auto* s : to_start)
        {
            if (handlers.spawn(run_stream(s))) continue;

            std::scoped_lock lock{mu};
            session.reset(s->id, http2::error_code::refused_stream);
            remove_stream(s->id);
            streams.erase(s->id);
            --running;
            refused = true;
        }

        // the refusals' RST_STREAMs go now, rather than with whatever's sent next.
        if (refused) wake_writer();
    }

    void http2_server_connection::schedule(http2_stream& stream)
    {
        // As Go's (its fix for CVE-2023-44487): a client resetting streams can open new ones while the handlers of
        // those it reset go on running, so handlers are limited as streams are; past the limit, they wait for others
        // to finish; and a client that keeps opening streams regardless is told to calm down.
        //
        // The connection's ended then: its GOAWAY's last stream is the last whose handler was started, as those queued
        // (and this one) never will be, so the client may retry them. It's shut down at once, though the handlers
        // still running hold the connection (and its max_connections slot) until they return (see run()).
        const auto limit = std::max<std::size_t>(options.session.local.max_concurrent_streams, 1);
        if (running + queued.size() >= 4 * limit)
        {
            logger.debug("HTTP/2 client opened too many streams while their handlers ran");
            session.goaway(last_started, http2::error_code::enhance_your_calm, "too many handlers");

            const auto id = stream.id;
            remove_stream(id);
            streams.erase(id); // stream, too
            end();
            return;
        }

        queued.push_back(&stream);
        promote();
    }

    void http2_server_connection::promote()
    {
        const auto limit = std::max<std::size_t>(options.session.local.max_concurrent_streams, 1);
        while (running < limit && !queued.empty() && !ending())
        {
            auto* s = queued.front();
            queued.pop_front();

            // a stream reset while it waited isn't started.
            if (s->failure)
            {
                remove_stream(s->id);
                streams.erase(s->id);
                continue;
            }

            ++running;
            last_started = std::max(last_started, s->id);
            started.push_back(s);
        }
    }

    coro::task<> http2_server_connection::finish_streams()
    {
        co_await handlers.join();

        // the connection's ending: handlers that hadn't started won't be.
        std::scoped_lock lock{mu};
        for (const auto* s : queued)
        {
            remove_stream(s->id);
            streams.erase(s->id);
        }
        queued.clear();
    }

    void http2_server_connection::on_headers(http2::stream_id              id,
                                             http2::block_kind             kind,
                                             std::span<const hpack::field> fields,
                                             bool                          end_stream)
    {
        if (kind != http2::block_kind::head)
        {
            // trailers end the request's body: kept (without those that can't be trailers) for the body's reader to
            // move to req.trailers once it reaches the end, so the handler finds them once it's read the body to its
            // end, and they don't change under it as it reads.
            if (const auto it = streams.find(id); it != streams.end() && end_stream)
            {
                auto& s = *it->second;
                for (const auto& f : fields)
                {
                    if (!trailer_allowed(f.name)) continue;
                    if (s.received_trailers == nullptr) s.received_trailers = std::make_unique<trailer_fields>();
                    s.received_trailers->add(f.name, f.value);
                }

                s.received_end = true;
                s.read_deadline.reset();
                s.changed.notify();
            }
            return;
        }

        if (ending()) return;

        auto stream = start_stream(id, fields, end_stream);
        if (stream == nullptr) return;

        idle_since.reset();
        add_stream(*stream);
        auto& s = *streams.emplace(id, std::move(stream)).first->second;
        schedule(s);
    }

    std::unique_ptr<http2_stream>
    http2_server_connection::start_stream(http2::stream_id id, std::span<const hpack::field> fields, bool end_stream)
    {
        auto  stream = std::make_unique<http2_stream>(this, id, pool);
        auto& req    = stream->req;

        std::string_view method;
        std::string_view path;
        std::string_view authority;
        std::string      cookie;

        for (const auto& f : fields)
        {
            if (f.name.starts_with(':'))
            {
                if (f.name == ":method") method = f.value;
                else if (f.name == ":path") path = f.value;
                else if (f.name == ":authority") authority = f.value;
                continue;
            }

            if (f.name != "cookie")
            {
                req.headers.add(f.name, f.value);
                continue;
            }

            // a cookie may arrive in pieces, joined for HTTP/1's semantics (RFC 9113, section 8.2.3).
            if (!cookie.empty()) cookie += "; ";
            cookie += f.value;
        }
        if (!cookie.empty()) req.headers.set("cookie"sv, std::move(cookie));

        req.method = parse_method(method);
        if (req.method == request_method::NONE)
        {
            refuse(id, status::NOT_IMPLEMENTED, end_stream);
            return nullptr;
        }

        // CONNECT's target is :authority (RFC 9113, section 8.5).
        const auto target = req.method == request_method::CONNECT ? authority : path;
        if (parse_request_target(req.method, target, req.target))
        {
            refuse(id, status::BAD_REQUEST, end_stream);
            return nullptr;
        }

        const auto host = authority.empty() ? req.headers.get("host"sv).value_or(""sv) : authority;
        if (!host.empty())
        {
            auto parsed = parse_host(host);
            if (!parsed.has_value())
            {
                refuse(id, status::BAD_REQUEST, end_stream);
                return nullptr;
            }
            req.host.assign(parsed->host);
        }
        if (!req.target.host.empty()) req.host.assign(req.target.host);

        req.version = {.major = 2, .minor = 0};

        // an expectation the server can't meet is refused, as over HTTP/1.1.
        const auto expected = expectation_of(req);
        if (expected == expectation::unknown)
        {
            refuse(id, status::EXPECTATION_FAILED, end_stream);
            return nullptr;
        }
        stream->expects_continue = expected == expectation::continue_100 && !end_stream;

        req.tls_version       = tls_version(conn);
        req.peer_certificates = peer_certs;
        auto body             = std::make_unique<stream_reader>(stream.get());
        stream->body          = body.get();
        stream->trailers_to   = &req.trailers;
        req.body              = std::move(body);

        // its handler's token: stopped when the stream fails (see fail_stream()), or by abort (at once, after
        // receiving, if it came first); and the server's abort token, for middleware to tell the two apart.
        stream->stop = std::stop_source{};
        req.stop     = stream->stop.get_token();
        req.aborted  = abort_token;
        if (aborted) to_stop.push_back(stream->stop);

        stream->head_request  = req.method == request_method::HEAD;
        stream->received_end  = end_stream;
        stream->read_timeout  = options.body_read_timeout;
        stream->write_timeout = options.write_timeout;

        auto& resp       = stream->resp;
        resp.version     = {.major = 2, .minor = 0};
        resp.status_code = status::NONE;
        resp.body        = &stream->writer;
        resp.headers.set("Date"sv, current_date());

        return stream;
    }

    void http2_server_connection::refuse(http2::stream_id id, status code, bool request_ended)
    {
        const auto                        text = std::to_string(static_cast<int>(code));
        const std::array<hpack::field, 2> head{
            hpack::field{.name = ":status",           .value = text},
            hpack::field{   .name = "date", .value = current_date()},
        };

        if (auto err = session.submit_headers(id, head, true))
        {
            logger.debug("failed to answer HTTP/2 stream {}: {}", id, err.message());
            return;
        }

        // the response is complete: the client needn't send the rest of its request (RFC 9113, section 8.1).
        if (!request_ended) session.reset(id, http2::error_code::no_error);
    }

    void http2_server_connection::on_goaway(http2::stream_id /*last*/, std::error_code err, std::string_view /*debug*/)
    {
        // the client's done opening streams: once those it has are, so is the connection.
        logger.debug("HTTP/2 client sent GOAWAY: {}", err.message());
        peer_goaway = true;
    }

    void http2_server_connection::on_ping_ack(std::span<const std::byte, 8> opaque)
    {
        if (draining && !final_sent && std::ranges::equal(opaque, drain_ping)) final_goaway();
    }

    coro::task<> http2_server_connection::run_stream(http2_stream* stream)
    {
        // NOTE: onto the pool, so the reader (which started it) goes on reading.
        co_await pool->schedule();

        response_writer rw{stream, &http2_ops, &stream->resp};

        // the client may wait for 100 Continue before it sends the body: it's sent when the handler first reads it.
        if (stream->expects_continue) stream->body->call_at_start(rw.expect_continue());

        std::exception_ptr failure;
        try
        {
            co_await handler(stream->req, rw);
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        // a response cut short isn't finished (which would end it as if it were complete), but reset.
        const bool complete = co_await respond_after(*stream, rw, failure);

        {
            std::scoped_lock lock{mu};

            // reset (so the client doesn't take it as complete) if it's not ended: with CANCEL if the handler was
            // stopped by abort (as a failed stream's is stopped only once it has failed, a stopped one that hasn't
            // was); and the client needn't send what the handler didn't read of the request.
            if (!stream->failure && (!complete || !stream->ended))
            {
                const bool aborted_handler = stream->stop.stop_requested();
                session.reset(stream->id,
                              aborted_handler ? http2::error_code::cancel : http2::error_code::internal_error);
            }
            else if (!stream->failure && !stream->received_end)
            {
                session.reset(stream->id, http2::error_code::no_error);
            }

            remove_stream(stream->id);
            streams.erase(stream->id);
            if (streams.empty()) idle_since = io::clock::now();

            // another handler may start in its place.
            --running;
            promote();
            maybe_end();
        }

        start_handlers();
        stop_pending(); // maybe_end() may have ended the connection
        wake_writer();
        wake_timer();
    }

    coro::task<bool>
    http2_server_connection::respond_after(http2_stream& stream, response_writer& rw, const std::exception_ptr& failure)
    {
        const auto& req = stream.req;

        // the handler was stopped: its stream failed (reset, timed out, or its connection ended), or abort. Either way,
        // no one wants an answer the handler didn't give (nor a 500 for one that threw, e.g. a database driver's own
        // "canceled", which isn't an error of the server's): the stream's reset, unless it failed already.
        const bool stopped = stream.stop.stop_requested();
        try
        {
            if (failure)
            {
                const auto level = stopped ? spdlog::level::debug : spdlog::level::err;
                try
                {
                    std::rethrow_exception(failure);
                }
                catch (const std::exception& ex)
                {
                    logger.log(level,
                               "handler for {} {} threw: {}",
                               method_string(req.method),
                               req.target.path,
                               ex.what());
                }
                catch (...)
                {
                    logger.log(level, "handler for {} {} threw", method_string(req.method), req.target.path);
                }

                // a response under way can't be finished: the client mustn't take what it has as all of it.
                if (rw.sent() || stopped) co_return false;

                // a 500 rather than the response the handler meant (perhaps after a 1xx), without its headers or
                // trailers.
                rw.headers().clear();
                rw.trailers().clear();
                rw.headers().set("Date"sv, current_date());
                co_await rw.send(status::INTERNAL_SERVER_ERROR, 0);
            }
            else if (!rw.sent())
            {
                if (stopped) co_return false;
                co_await rw.send(status::OK, 0);
            }

            // less than its Content-Length (a response to HEAD has none, whatever its head says).
            if (!stream.head_request && rw.body_remaining() > 0) co_return false;

            // e.g. a trailer that can't be sent: then the stream's reset (not ended), so the client doesn't take the
            // response for complete.
            if (auto finished = co_await rw.finish(); finished.err)
            {
                if (finished.err == std::errc::invalid_argument)
                {
                    logger.warn("handler for {} {} set a trailer that can't be sent",
                                method_string(req.method),
                                req.target.path);
                    co_return false;
                }
                logger.debug("failed to end the response to {} {}: {}",
                             method_string(req.method),
                             req.target.path,
                             finished.err.message());
            }
        }
        catch (const std::exception& ex)
        {
            logger.debug("failed to finish the response to {} {}: {}",
                         method_string(req.method),
                         req.target.path,
                         ex.what());
        }

        co_return true;
    }

    std::optional<io::clock::time_point> http2_server_connection::check_connection(io::clock::time_point now)
    {
        std::optional<io::clock::time_point> next;
        if (drain_deadline.has_value())
        {
            if (*drain_deadline <= now && !final_sent) final_goaway();
            else next = drain_deadline;
        }
        if (ending()) return std::nullopt;

        // the preface and SETTINGS, and then a connection without streams, may only take so long.
        const auto timeout = options.header_read_timeout;
        if (timeout <= std::chrono::seconds::zero()) return next;

        const auto sooner = [&](io::clock::time_point t) { return next.has_value() ? std::min(*next, t) : t; };

        if (!session.has_peer_settings())
        {
            if (now - started_at < timeout) return sooner(started_at + timeout);

            logger.debug("HTTP/2 client didn't send its preface in time");
            end();
            return std::nullopt;
        }

        if (streams.empty() && !idle_since.has_value()) idle_since = now;
        if (!idle_since.has_value()) return next;
        if (now - *idle_since < timeout) return sooner(*idle_since + timeout);

        session.goaway(session.last_peer_stream(), http2::error_code::no_error);
        end();
        return std::nullopt;
    }

    void http2_server_connection::begin_drain()
    {
        if (draining || ending()) return;

        draining = true;
        session.goaway(http2::max_stream_id, http2::error_code::no_error);
        session.ping(drain_ping);
        drain_deadline = io::clock::now() + drain_wait;
        wake_timer();
    }

    void http2_server_connection::final_goaway()
    {
        final_sent = true;
        drain_deadline.reset();
        session.goaway(session.last_peer_stream(), http2::error_code::no_error);
        maybe_end();
    }

    void http2_server_connection::maybe_end()
    {
        if (ending()) return;

        const bool done = session.failed() || ((final_sent || peer_goaway) && streams.empty());
        if (done) end();
    }

}

coro::task<> serve_http2(io::scheduler*             scheduler,
                         connection&                conn,
                         std::span<const std::byte> already_read,
                         const http2_options&       options,
                         const request_handler&     handler,
                         spdlog::logger&            logger,
                         std::stop_token            stop,
                         std::stop_token            abort)
{
    http2_server_connection c{scheduler, conn, options, handler, logger};
    co_await c.serve(already_read, std::move(stop), std::move(abort));
}

}
