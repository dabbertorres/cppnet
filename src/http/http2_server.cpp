#include "http/http2_server.hpp"

#include <array>
#include <chrono>
#include <cstddef>
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

#include <spdlog/logger.h>

#include "coro/async_scope.hpp"
#include "coro/task.hpp"
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
#include "io/scheduler.hpp"
#include "io/writer.hpp"

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
        {}

        coro::task<> serve(std::span<const std::byte> already_read, std::stop_token stop);

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

        coro::task<> run_stream(http2_stream* stream);

        // begin_drain starts a graceful shutdown: GOAWAY, then once the client has seen it (a PING), the final
        // GOAWAY.
        void begin_drain();
        void final_goaway();

        const http2_options&   options;
        const request_handler& handler;

        // Guarded by mu.
        std::unordered_map<http2::stream_id, std::unique_ptr<http2_stream>> streams;
        std::vector<http2_stream*>                                          started; // to start once unlocked
        bool                                                                draining    = false;
        bool                                                                final_sent  = false; // the final GOAWAY
        bool                                                                peer_goaway = false;
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

            // a response to HEAD has no body, whatever its head says.
            const bool end = end_stream || (stream.head_request && !status_is_information(resp.status_code));

            stream.status_text = std::to_string(static_cast<int>(resp.status_code));
            stream.fields.clear();
            stream.fields.push_back({.name = ":status", .value = stream.status_text});
            append_fields(resp.headers, stream.names, stream.fields);

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
                stream.ended = true;
                if (auto sent = c.session.submit_data(stream.id, {}, true); !sent.has_value())
                    return coro::ready_task<io::result>(io::result{.err = sent.error()});
            }
        }

        c.wake_writer();
        return coro::ready_task<io::result>();
    }

}

const http2_stream_ops http2_ops{.send_head = http2_send_head, .finish = http2_finish};

namespace
{

    coro::task<> http2_server_connection::serve(std::span<const std::byte> already_read, std::stop_token stop)
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

        co_await run(already_read);
    }

    void http2_server_connection::after_receive()
    {
        std::vector<http2_stream*> to_start;
        {
            std::scoped_lock lock{mu};
            to_start.swap(started);
        }

        for (auto* s : to_start)
        {
            if (!handlers.spawn(run_stream(s)))
            {
                std::scoped_lock lock{mu};
                session.reset(s->id, http2::error_code::refused_stream);
                remove_stream(s->id);
                streams.erase(s->id);
            }
        }
    }

    coro::task<> http2_server_connection::finish_streams() { co_await handlers.join(); }

    void http2_server_connection::on_headers(http2::stream_id              id,
                                             http2::block_kind             kind,
                                             std::span<const hpack::field> fields,
                                             bool                          end_stream)
    {
        if (kind != http2::block_kind::head)
        {
            // trailers end the request's body (and aren't passed on).
            if (const auto it = streams.find(id); it != streams.end() && end_stream)
            {
                it->second->received_end = true;
                it->second->changed.notify();
            }
            return;
        }

        auto stream = start_stream(id, fields, end_stream);
        if (stream == nullptr) return;

        idle_since.reset();
        started.push_back(stream.get());
        add_stream(*stream);
        streams.emplace(id, std::move(stream));
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
        req.tls     = is_tls(conn);
        req.body    = std::make_unique<stream_reader>(stream.get());

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
        const std::array<hpack::field, 1> head{
            hpack::field{.name = ":status", .value = text}
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

        auto&           req = stream->req;
        response_writer rw{stream, &http2_ops, &stream->resp};

        std::exception_ptr failure;
        try
        {
            co_await handler(req, rw);
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        try
        {
            if (failure)
            {
                try
                {
                    std::rethrow_exception(failure);
                }
                catch (const std::exception& ex)
                {
                    logger.error("handler for {} {} threw: {}", method_string(req.method), req.target.path, ex.what());
                }
                catch (...)
                {
                    logger.error("handler for {} {} threw", method_string(req.method), req.target.path);
                }

                if (rw.status_code() == status::NONE) co_await rw.send(status::INTERNAL_SERVER_ERROR, 0);
            }
            else if (!rw.sent())
            {
                co_await rw.send(status::OK, 0);
            }

            (void)co_await rw.finish();
        }
        catch (const std::exception& ex)
        {
            logger.debug("failed to finish the response to {} {}: {}",
                         method_string(req.method),
                         req.target.path,
                         ex.what());
        }

        {
            std::scoped_lock lock{mu};

            // a response cut short (the handler threw, or wrote less than its Content-Length) is reset, so the client
            // doesn't take it as complete; and the client needn't send what the handler didn't read of the request.
            const bool short_body = !stream->head_request && rw.body_remaining() > 0;
            if (!stream->failure && (failure || short_body || !stream->ended))
                session.reset(stream->id, http2::error_code::internal_error);
            else if (!stream->failure && !stream->received_end) session.reset(stream->id, http2::error_code::no_error);

            remove_stream(stream->id);
            streams.erase(stream->id);
            if (streams.empty()) idle_since = io::clock::now();
            maybe_end();
        }

        wake_writer();
        wake_timer();
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
                         std::stop_token            stop)
{
    http2_server_connection c{scheduler, conn, options, handler, logger};
    co_await c.serve(already_read, std::move(stop));
}

}
