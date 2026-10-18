#include "http/http2_server.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
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
#include "coro/notifier.hpp"
#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http.hpp"
#include "http/http2.hpp"
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
#include "tcp.hpp"
#include "util/string_util.hpp"

namespace net::http::detail
{

namespace
{

    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    // How much is read from the connection at once.
    constexpr std::size_t read_size = std::size_t{32} * 1'024;

    // How much output may be waiting for the writer before streams' body writes wait for it to catch up.
    constexpr std::size_t max_output = std::size_t{1} << 20;

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

    // connection_specific is whether a field is HTTP/1's connection management, which HTTP/2 mustn't send (RFC 9113,
    // section 8.2.2).
    bool connection_specific(std::string_view name) noexcept
    {
        return util::equal_ignore_case(name, "connection"sv) || util::equal_ignore_case(name, "keep-alive"sv)
            || util::equal_ignore_case(name, "proxy-connection"sv)
            || util::equal_ignore_case(name, "transfer-encoding"sv) || util::equal_ignore_case(name, "upgrade"sv);
    }

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

    std::optional<io::clock::time_point> after(std::chrono::seconds timeout)
    {
        if (timeout <= std::chrono::seconds::zero()) return std::nullopt;
        return io::clock::now() + timeout;
    }

}

class http2_connection;

// request_body reads a stream's request body, as its DATA arrives, giving the client back its windows as it goes.
// NOLINTNEXTLINE(misc-use-internal-linkage): a member of http2_stream, which response.hpp declares.
class request_body final : public io::reader
{
public:
    explicit request_body(http2_stream* stream) noexcept
        : stream{stream}
    {}

    coro::task<io::result> read(std::span<std::byte> data) override;
    using io::reader::read;

    [[nodiscard]] io::handle native_handle() const noexcept override;

private:
    http2_stream* stream;
};

// response_body writes a stream's response body as DATA, waiting for the flow-control windows to have room.
// NOLINTNEXTLINE(misc-use-internal-linkage): a member of http2_stream, which response.hpp declares.
class response_body final : public io::writer
{
public:
    explicit response_body(http2_stream* stream) noexcept
        : stream{stream}
    {}

    coro::task<io::result> write(std::span<const std::byte> data) override;
    using io::writer::write;

    [[nodiscard]] io::handle native_handle() const noexcept override;

private:
    http2_stream* stream;
};

class http2_stream
{
public:
    http2_stream(http2_connection* conn, http2::stream_id id, coro::thread_pool* pool)
        : conn{conn}
        , id{id}
        , changed{pool}
        , body_out{this}
    {}

    http2_connection* conn;
    http2::stream_id  id;

    server_request  req;
    server_response resp;

    // Guarded by the connection's lock.
    std::vector<std::byte>               received; // the request body's data not read yet, from received_start
    std::size_t                          received_start = 0;
    bool                                 received_end   = false;
    bool                                 ended          = false; // the response's END_STREAM has been sent
    std::error_code                      failure;                // reset, timed out, or the connection ended
    std::optional<io::clock::time_point> deadline;               // of the wait in progress, if any

    bool head_request = false;

    // notified whenever any of the above changes, or the stream's (or connection's) window grows.
    coro::notifier changed;

    response_body      body_out;
    io::discard_writer discard;

    // for sending heads, reused.
    std::string               status_text;
    std::vector<std::string>  names;
    std::vector<hpack::field> fields;
};

class http2_connection final : private http2::session_events
{
public:
    http2_connection(io::scheduler*         scheduler,
                     connection&            conn,
                     const http2_options&   options,
                     const request_handler& handler,
                     spdlog::logger&        logger)
        : session{http2::role::server, options.session}
        , scheduler{scheduler}
        , pool{scheduler->pool()}
        , conn{conn}
        , options{options}
        , handler{handler}
        , logger{logger}
        , writer_wakeup{pool}
        , drained{pool}
    {}

    http2_connection(const http2_connection&)            = delete;
    http2_connection& operator=(const http2_connection&) = delete;
    http2_connection(http2_connection&&)                 = delete;
    http2_connection& operator=(http2_connection&&)      = delete;
    ~http2_connection() override                         = default;

    coro::task<> serve(std::span<const std::byte> already_read, std::stop_token stop);

    // Used by streams (see request_body, response_body, and http2_send_head), each holding the lock.
    std::mutex     mu;
    http2::session session;

    // wake_writer tells the writer there's output, if there is: called after unlocking.
    void wake_writer()
    {
        bool output = false;
        {
            std::scoped_lock lock{mu};
            output = session.has_output();
        }
        if (output) writer_wakeup.notify();
    }

    // set_deadline sets a stream's wait's deadline (holding the lock), telling the timer if it's sooner than it knew.
    void set_deadline(http2_stream& s, std::chrono::seconds timeout)
    {
        s.deadline = after(timeout);
        if (s.deadline.has_value() && (!timer_target.has_value() || *s.deadline < *timer_target)) wake_timer();
    }

    [[nodiscard]] std::size_t output_size() const noexcept { return session.output_size(); }

    coro::notifier& output_drained() noexcept { return drained; }

    [[nodiscard]] io::handle native_handle() const noexcept { return detail::native_handle(conn); }

    [[nodiscard]] const http2_options& config() const noexcept { return options; }

private:
    // The session's events, with the lock held.
    void on_headers(http2::stream_id              id,
                    http2::block_kind             kind,
                    std::span<const hpack::field> fields,
                    bool                          end_stream) override;
    void on_data(http2::stream_id id, std::span<const std::byte> data, bool end_stream) override;
    void on_stream_reset(http2::stream_id id, std::error_code err) override;
    void on_window_update(http2::stream_id id) override;
    void on_settings(const http2::settings& peer) override;
    void on_goaway(http2::stream_id last, std::error_code err, std::string_view debug) override;
    void on_ping_ack(std::span<const std::byte, 8> opaque) override;

    // start_stream makes a stream for a request's head, to start its handler, or answers it (e.g. a method that isn't
    // implemented), returning nullptr then.
    std::unique_ptr<http2_stream>
    start_stream(http2::stream_id id, std::span<const hpack::field> fields, bool end_stream);

    // refuse answers a request with just a status, ending the stream.
    void refuse(http2::stream_id id, status code, bool request_ended);

    // receive passes what's been read to the session, returning how much it used, or false if the connection's ended.
    bool receive(std::vector<std::byte>& pending);

    coro::task<> run_stream(http2_stream* stream);
    coro::task<> write_loop();
    coro::task<> timer_loop();

    // check_timers acts on what's due (holding the lock), returning the next deadline: the streams' timeouts, and the
    // connection's (its preface's, and its being idle).
    std::optional<io::clock::time_point> check_timers();
    std::optional<io::clock::time_point> check_streams(io::clock::time_point now);
    std::optional<io::clock::time_point> check_connection(io::clock::time_point now);

    // begin_drain starts a graceful shutdown: GOAWAY, then once the client has seen it (a PING), the final GOAWAY.
    void begin_drain();
    void final_goaway();

    // end ends the connection (holding the lock): its streams fail, the writer finishes, and the reader stops.
    void end();

    // maybe_end ends the connection (holding the lock) if it's done: drained, or GOAWAY'd, with no streams left.
    void maybe_end();

    void wake_timer();

    io::scheduler*         scheduler;
    coro::thread_pool*     pool;
    connection&            conn;
    const http2_options&   options;
    const request_handler& handler;
    spdlog::logger&        logger;

    // Guarded by mu.
    std::unordered_map<http2::stream_id, std::unique_ptr<http2_stream>> streams;
    std::vector<http2_stream*>                                          started; // to start once unlocked
    bool                                                                ending      = false;
    bool                                                                draining    = false;
    bool                                                                final_sent  = false; // the final GOAWAY
    bool                                                                peer_goaway = false;
    std::optional<io::clock::time_point>                                drain_deadline;
    std::optional<io::clock::time_point>                                idle_since;
    std::optional<io::clock::time_point>                                timer_target;
    io::clock::time_point                                               started_at = io::clock::now();

    coro::notifier             writer_wakeup; // output to write, or the connection's ending
    coro::notifier             drained;       // the writer has written some
    std::atomic<std::uint64_t> timer_word{0}; // the timer's, for scheduler::wait_on()

    coro::async_scope handlers; // the streams' handlers
    coro::async_scope workers;  // the writer and the timer
};

// --- streams' bodies and heads

coro::task<io::result> request_body::read(std::span<std::byte> data)
{
    auto& c = *stream->conn;
    while (true)
    {
        std::uint64_t seen = 0;
        {
            std::scoped_lock lock{c.mu};
            auto&            s = *stream;

            if (s.received_start < s.received.size())
            {
                const auto n = std::min(data.size(), s.received.size() - s.received_start);
                std::ranges::copy(std::span{s.received}.subspan(s.received_start, n), data.begin());
                s.received_start += n;
                if (s.received_start == s.received.size())
                {
                    s.received.clear();
                    s.received_start = 0;
                }

                // the client may send as much more.
                c.session.consume(s.id, n);
                s.deadline.reset();
                co_return io::result{.count = n};
            }

            if (s.received_end) co_return io::result{.err = make_error_code(io::errc::end_of_stream)};
            if (s.failure) co_return io::result{.err = s.failure};

            seen = s.changed.value();
            c.set_deadline(s, c.config().body_read_timeout);
        }

        c.wake_writer();
        (void)co_await stream->changed.wait(seen);
    }
}

io::handle request_body::native_handle() const noexcept { return stream->conn->native_handle(); }

coro::task<io::result> response_body::write(std::span<const std::byte> data)
{
    auto&       c       = *stream->conn;
    std::size_t written = 0;

    while (true)
    {
        bool          wait_for_writer = false;
        std::uint64_t seen            = 0;
        {
            std::scoped_lock lock{c.mu};
            auto&            s = *stream;

            if (s.failure) co_return io::result{.count = written, .err = s.failure};

            if (c.output_size() > max_output)
            {
                // the writer's behind (the client reads slowly): wait for it, rather than queueing more.
                wait_for_writer = true;
                seen            = c.output_drained().value();
            }
            else
            {
                const auto sent = c.session.submit_data(s.id, data.subspan(written), false);
                if (!sent.has_value())
                {
                    s.failure = sent.error();
                    co_return io::result{.count = written, .err = s.failure};
                }

                written += *sent;
                if (written == data.size())
                {
                    s.deadline.reset();
                }
                else
                {
                    // no window: wait for the client to give some back.
                    seen = s.changed.value();
                    c.set_deadline(s, c.config().write_timeout);
                }
            }
        }

        c.wake_writer();
        if (written == data.size()) co_return io::result{.count = written};

        if (wait_for_writer) (void)co_await c.output_drained().wait(seen);
        else (void)co_await stream->changed.wait(seen);
    }
}

io::handle response_body::native_handle() const noexcept { return stream->conn->native_handle(); }

namespace
{

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
            stream.names.clear();
            stream.fields.clear();
            stream.fields.push_back({.name = ":status", .value = stream.status_text});

            // names are lowercase in HTTP/2; connection-specific fields are dropped.
            for (const auto& [name, values] : resp.headers)
            {
                if (connection_specific(name)) continue;

                auto& lowered = stream.names.emplace_back(name);
                std::ranges::transform(lowered, lowered.begin(), [](char ch) { return util::to_lower_ascii(ch); });
            }

            std::size_t i = 0;
            for (const auto& [name, values] : resp.headers)
            {
                if (connection_specific(name)) continue;
                const std::string_view lowered = stream.names[i++];
                for (const auto& value : values)
                    stream.fields.push_back(
                        {.name = lowered, .value = value, .never_indexed = hpack::sensitive(lowered)});
            }

            if (auto err = c.session.submit_headers(stream.id, stream.fields, end))
                return coro::ready_task<std::expected<io::writer*, std::error_code>>(std::unexpected(err));

            if (end) stream.ended = true;
        }

        c.wake_writer();
        io::writer* body = stream.ended ? static_cast<io::writer*>(&stream.discard) : &stream.body_out;
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

// --- the connection

coro::task<> http2_connection::serve(std::span<const std::byte> already_read, std::stop_token stop)
{
    // close() (stop) ends the connection gracefully.
    std::stop_callback on_stop{stop,
                               [this] noexcept
                               {
                                   {
                                       std::scoped_lock lock{mu};
                                       begin_drain();
                                   }
                                   writer_wakeup.notify();
                               }};

    (void)workers.spawn(write_loop());
    (void)workers.spawn(timer_loop());

    std::vector<std::byte> pending(already_read.begin(), already_read.end());
    std::vector<std::byte> buffer(read_size);

    auto& reader = as_reader(conn);
    while (receive(pending))
    {
        auto res = co_await reader.read(buffer);
        pending.insert(pending.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(res.count));
        if (res.err)
        {
            // what was read before the end is still received.
            if (!pending.empty()) (void)receive(pending);
            break;
        }
    }

    {
        std::scoped_lock lock{mu};
        end();
    }

    co_await handlers.join();

    // the writer writes what's left (e.g. GOAWAY), then finishes; and so does the timer.
    writer_wakeup.close();
    wake_timer();
    co_await workers.join();
}

bool http2_connection::receive(std::vector<std::byte>& pending)
{
    std::vector<http2_stream*> to_start;
    bool                       go_on = true;
    {
        std::scoped_lock lock{mu};
        if (ending) return false;

        const auto used = session.receive(pending, *this, io::clock::now());
        if (!used.has_value())
        {
            logger.debug("HTTP/2 connection error: {}", used.error().message());
            end();
            go_on = false;
        }
        else
        {
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(*used));
        }

        to_start.swap(started);
        maybe_end();
        go_on = go_on && !ending;
    }

    writer_wakeup.notify();

    for (auto* s : to_start)
    {
        if (!handlers.spawn(run_stream(s)))
        {
            std::scoped_lock lock{mu};
            session.reset(s->id, http2::error_code::refused_stream);
            streams.erase(s->id);
        }
    }

    return go_on;
}

void http2_connection::on_headers(http2::stream_id              id,
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
    streams.emplace(id, std::move(stream));
}

std::unique_ptr<http2_stream>
http2_connection::start_stream(http2::stream_id id, std::span<const hpack::field> fields, bool end_stream)
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
    req.body    = std::make_unique<request_body>(stream.get());

    stream->head_request = req.method == request_method::HEAD;
    stream->received_end = end_stream;

    auto& resp       = stream->resp;
    resp.version     = {.major = 2, .minor = 0};
    resp.status_code = status::NONE;
    resp.body        = &stream->body_out;
    resp.headers.set("Date"sv, current_date());

    return stream;
}

void http2_connection::refuse(http2::stream_id id, status code, bool request_ended)
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

void http2_connection::on_data(http2::stream_id id, std::span<const std::byte> data, bool end_stream)
{
    const auto it = streams.find(id);
    if (it == streams.end())
    {
        // the handler's done with the stream: what it doesn't read, the client gets back.
        session.consume(id, data.size());
        return;
    }

    auto& s = *it->second;
    s.received.insert(s.received.end(), data.begin(), data.end());
    if (end_stream) s.received_end = true;
    s.changed.notify();
}

void http2_connection::on_stream_reset(http2::stream_id id, std::error_code err)
{
    const auto it = streams.find(id);
    if (it == streams.end()) return;

    if (!it->second->failure) it->second->failure = err;
    it->second->changed.notify();
}

void http2_connection::on_window_update(http2::stream_id id)
{
    if (id != 0)
    {
        if (const auto it = streams.find(id); it != streams.end()) it->second->changed.notify();
        return;
    }

    for (auto& [_, s] : streams) s->changed.notify();
}

void http2_connection::on_settings(const http2::settings& /*peer*/)
{
    // e.g. INITIAL_WINDOW_SIZE, which moves every stream's window.
    for (auto& [_, s] : streams) s->changed.notify();
}

void http2_connection::on_goaway(http2::stream_id /*last*/, std::error_code err, std::string_view /*debug*/)
{
    // the client's done opening streams: once those it has are, so is the connection.
    logger.debug("HTTP/2 client sent GOAWAY: {}", err.message());
    peer_goaway = true;
}

void http2_connection::on_ping_ack(std::span<const std::byte, 8> opaque)
{
    if (draining && !final_sent && std::ranges::equal(opaque, drain_ping)) final_goaway();
}

coro::task<> http2_connection::run_stream(http2_stream* stream)
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

        streams.erase(stream->id);
        if (streams.empty()) idle_since = io::clock::now();
        maybe_end();
    }

    writer_wakeup.notify();
    wake_timer();
}

coro::task<> http2_connection::write_loop()
{
    co_await pool->schedule();

    auto&                  writer = as_writer(conn);
    std::vector<std::byte> out;

    while (true)
    {
        const auto seen = writer_wakeup.value();
        {
            std::scoped_lock lock{mu};
            session.take_output(out);
        }

        if (!out.empty())
        {
            const auto res = co_await io::write_all(writer, out);
            drained.notify();
            if (res.err)
            {
                logger.debug("HTTP/2 connection's write failed: {}", res.err.message());
                std::scoped_lock lock{mu};
                end();
                break;
            }
            continue;
        }

        if (!co_await writer_wakeup.wait(seen))
        {
            // closed: what's left is written (e.g. GOAWAY), and then the writer's done.
            {
                std::scoped_lock lock{mu};
                session.take_output(out);
            }
            if (!out.empty()) (void)co_await io::write_all(writer, out);
            break;
        }
    }

    drained.close();
}

coro::task<> http2_connection::timer_loop()
{
    co_await pool->schedule();

    while (true)
    {
        std::optional<io::clock::time_point> next;
        std::uint64_t                        seen = 0;
        {
            std::scoped_lock lock{mu};
            if (ending) break;

            seen         = timer_word.load(std::memory_order::acquire);
            next         = check_timers();
            timer_target = next;
            if (ending) break;
        }

        writer_wakeup.notify();

        // stopping the scheduler ends the wait (and the connection, as its reads fail).
        const auto err = co_await scheduler->wait_on(timer_word, seen, next);
        if (err == std::errc::operation_canceled) break;
    }
}

std::optional<io::clock::time_point> http2_connection::check_timers()
{
    const auto now  = io::clock::now();
    auto       next = check_streams(now);

    const auto consider = [&](std::optional<io::clock::time_point> t)
    {
        if (t.has_value() && (!next.has_value() || *t < *next)) next = t;
    };

    consider(check_connection(now));
    if (ending) return std::nullopt;

    if (drain_deadline.has_value())
    {
        if (*drain_deadline <= now && !final_sent) final_goaway();
        else consider(drain_deadline);
    }

    return next;
}

std::optional<io::clock::time_point> http2_connection::check_streams(io::clock::time_point now)
{
    // a stream waiting too long for its body, or to send its response, is reset alone.
    std::optional<io::clock::time_point> next;
    for (auto& [id, s] : streams)
    {
        if (!s->deadline.has_value()) continue;
        if (*s->deadline > now)
        {
            if (!next.has_value() || *s->deadline < *next) next = s->deadline;
            continue;
        }

        logger.debug("HTTP/2 stream {} timed out", id);
        s->deadline.reset();
        s->failure = std::make_error_code(std::errc::timed_out);
        session.reset(id, http2::error_code::cancel);
        s->changed.notify();
    }

    return next;
}

std::optional<io::clock::time_point> http2_connection::check_connection(io::clock::time_point now)
{
    // the preface and SETTINGS, and then a connection without streams, may only take so long.
    const auto timeout = options.header_read_timeout;
    if (timeout <= std::chrono::seconds::zero()) return std::nullopt;

    if (!session.has_peer_settings())
    {
        if (now - started_at < timeout) return started_at + timeout;

        logger.debug("HTTP/2 client didn't send its preface in time");
        end();
        return std::nullopt;
    }

    if (streams.empty() && !idle_since.has_value()) idle_since = now;
    if (!idle_since.has_value()) return std::nullopt;
    if (now - *idle_since < timeout) return *idle_since + timeout;

    session.goaway(session.last_peer_stream(), http2::error_code::no_error);
    end();
    return std::nullopt;
}

void http2_connection::begin_drain()
{
    if (draining || ending) return;

    draining = true;
    session.goaway(http2::max_stream_id, http2::error_code::no_error);
    session.ping(drain_ping);
    drain_deadline = io::clock::now() + drain_wait;
    wake_timer();
}

void http2_connection::final_goaway()
{
    final_sent = true;
    drain_deadline.reset();
    session.goaway(session.last_peer_stream(), http2::error_code::no_error);
    maybe_end();
}

void http2_connection::maybe_end()
{
    if (ending) return;

    const bool done = session.failed() || ((final_sent || peer_goaway) && streams.empty());
    if (done) end();
}

void http2_connection::end()
{
    if (ending) return;
    ending = true;

    for (auto& [_, s] : streams)
    {
        if (!s->failure) s->failure = std::make_error_code(std::errc::connection_aborted);
        s->changed.notify();
    }

    // the reader stops (finding the end of the stream), the writer writes what's left, and the timer stops.
    socket_of(conn).shutdown_read();
    writer_wakeup.notify();
    wake_timer();
}

void http2_connection::wake_timer()
{
    timer_word.fetch_add(1, std::memory_order::acq_rel);
    scheduler->notify_all(timer_word);
}

coro::task<> serve_http2(io::scheduler*             scheduler,
                         connection&                conn,
                         std::span<const std::byte> already_read,
                         const http2_options&       options,
                         const request_handler&     handler,
                         spdlog::logger&            logger,
                         std::stop_token            stop)
{
    http2_connection c{scheduler, conn, options, handler, logger};
    co_await c.serve(already_read, std::move(stop));
}

}
