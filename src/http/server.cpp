#include "http/server.hpp"

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
#include <new>
#include <optional>
#include <ratio>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/common.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "exception.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
#include "http/http2_server.hpp"
#include "http/origin.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/tunnel.hpp"
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
#include "tls/version.hpp"
#include "util/defer.hpp"

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

// preface_known is whether what's buffered starts with HTTP/2's preface, if it's enough to tell.
std::optional<bool> preface_known(std::span<const std::byte> buffered)
{
    const auto preface = std::as_bytes(std::span{net::http::http2::preface});
    const auto n       = std::min(buffered.size(), preface.size());
    if (!std::ranges::equal(buffered.first(n), preface.first(n))) return false;
    if (n == preface.size()) return true;
    return std::nullopt;
}

// preface_after finishes starts_with_preface, once filling reader (which needed a coroutine) does.
net::coro::task<bool> preface_after(net::io::buffered_reader& reader, net::coro::task<std::error_code> filling)
{
    if (co_await std::move(filling)) co_return false;

    while (true)
    {
        if (const auto known = preface_known(reader.buffered())) co_return *known;
        if (co_await reader.fill()) co_return false;
    }
}

// starts_with_preface reads (into reader's buffer, without consuming it) until it can tell whether the connection
// starts with HTTP/2's preface.
net::coro::task<bool> starts_with_preface(net::io::buffered_reader& reader)
{
    // usually, the client's first bytes have arrived, so the reads don't wait: then there's no need for a coroutine
    // (see net::coro::ready_task).
    while (true)
    {
        if (const auto known = preface_known(reader.buffered())) return net::coro::ready_task<bool>(*known);

        auto filling = reader.fill();
        if (!filling.finished_without_coroutine()) return preface_after(reader, std::move(filling));
        if (std::move(filling).result()) return net::coro::ready_task<bool>(false);
    }
}

// http2_session_config is HTTP/2's settings for a server's config: its streams' limit, and its head's, as a header list
// (whose fields count 32 more each, for at most as many fields as HTTP/1.1's); whether it takes extended CONNECT (for
// WebSockets); and the connection's window, which holds every stream's (at least 1 MiB), so bodies read one at a time
// don't wait for each other.
net::http::http2::session_config http2_session_config(const net::http::server_config& cfg)
{
    net::http::http2::session_config config;
    config.local.max_concurrent_streams  = cfg.http2_max_concurrent_streams;
    config.local.enable_connect_protocol = cfg.http2_websockets;

    // NOTE: saturating, as max_header_bytes may be near SIZE_MAX (no limit, in effect).
    constexpr std::size_t field_overhead = 32 * net::http::http11::max_fields;
    constexpr std::size_t most           = net::http::http2::unlimited - 1;
    config.local.max_header_list_size    = static_cast<std::uint32_t>(
        cfg.max_header_bytes >= most - field_overhead ? most : cfg.max_header_bytes + field_overhead);

    const auto streams       = std::uint64_t{config.local.max_concurrent_streams} * config.local.initial_window_size;
    config.connection_window = static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>(streams, std::uint64_t{1} << 20, net::http::http2::max_window));
    return config;
}

// http2_origins_of is cfg's http2_origins, normalized and without repeats, throwing std::invalid_argument for one that
// isn't an https origin, or (with TLS) whose host its certificate doesn't cover.
std::vector<std::string> http2_origins_of(const net::http::server_config& cfg)
{
    std::vector<std::string> out;
    for (const auto& text : cfg.http2_origins)
    {
        auto o = net::http::detail::parse_origin(text);
        if (!o.has_value()) throw std::invalid_argument{"not an https origin: " + text};

#ifdef NET_HAS_TLS
        if (cfg.tls.has_value() && !net::tls::detail::covers(*cfg.tls, o->host))
            throw std::invalid_argument{"the certificate isn't for " + text};
#endif

        if (!std::ranges::contains(out, o->serialized)) out.push_back(std::move(o->serialized));
    }
    return out;
}

// has_close is whether fields say the connection closes after this message.
bool has_close(const net::http::headers& fields) { return fields.has_token("Connection"sv, "close"sv); }

// wrote_less is whether the handler wrote less of the response's body than its Content-Length said: not for HEAD,
// whose body isn't sent.
bool wrote_less(const net::http::server_request& req, const net::http::response_writer& rw) noexcept
{
    return req.method != net::http::request_method::HEAD && rw.body_remaining() > 0;
}

// response_version is the version of a response to a request of version: the highest the server implements, which the
// client does too (RFC 9110, section 2.5).
net::http::protocol_version response_version(net::http::protocol_version version) noexcept
{
    return {.major = 1, .minor = version.major == 1 && version.minor == 0 ? 0U : 1U};
}

// sent_after is true once sending (which needed a coroutine) has sent a response.
net::coro::task<bool> sent_after(net::coro::task<net::io::writer*> sending)
{
    (void)co_await std::move(sending);
    co_return true;
}

// body_to_come is whether an HTTP/1.1 request has a body to read: chunked, or with a Content-Length over 0 (which
// decoding checked).
bool body_to_come(const net::http::server_request& req)
{
    return req.headers.get_all("Transfer-Encoding"sv).has_value() || req.headers.get_content_length().value_or(0) > 0;
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
          .priorities          = cfg.http2_priorities,
          .notsent_lowat       = cfg.http2_notsent_lowat,
          .writable            = cfg.http2_writable,
          .origins             = http2_origins_of(cfg),
      }
#ifdef NET_HAS_TLS
    , tls_ctx{cfg.tls}
#endif
{
    // NOTE: listening from now, rather than from when serve() starts (on a worker), so connections made in between are
    //       queued (up to max_pending_connections) rather than refused.
    if (!this->listener.listening()) this->listener.listen(max_pending_connections);

    // NOTE: once the rest is ready, as it aborts the server at once if the scheduler's stopped already.
    on_scheduler_stop.emplace(scheduler->stop_token(), abort_on_stop{this});
}

server::~server()
{
    // NOTE: first, waiting for a call in progress, so the scheduler stopping doesn't abort the server as it's
    //       destroyed.
    on_scheduler_stop.reset();

    // nothing will read what the handlers would answer, and they must finish before the server's gone.
    abort();

    // serve() and the connections use the server until they finish.
    running.close();
    running.join_blocking();
}

void server::close()
{
    // NOTE: whether or not serve() has started: it may yet be, as a task the scheduler hasn't run, and then returns at
    //       once.
    if (closed.exchange(true, std::memory_order::acq_rel)) return;

    listener.shutdown();

    // ends the connections (gracefully), and TLS handshakes, being served: and any yet to start, as requesting stop
    // runs the callbacks registered after it straight away.
    closing.request_stop();

    // serve() may be waiting for a connection to close (see max_connections).
    scheduler->notify_all(open_connections);
    logger->flush();
}

void server::abort()
{
    // the handlers' tokens, and HTTP/1.x requests in progress, which close() lets finish (see serve_requests()).
    // NOTE: before close(), so what both reach sees the abort first: e.g. a WebSocket's read, which close() would
    //       otherwise wake (on another thread) to send a close saying going away, before abort()'s cancellation came.
    // NOTE: holding no lock: the callbacks run here, and handlers' are anyone's.
    aborting.request_stop();

    close();
}

coro::task<> server::join()
{
    // NOTE: nothing of the server is touched once it's resumed, as the server may be destroyed as soon as the scope's
    //       empty.
    co_await running.join();
}

coro::task<> server::serve()
{
    // the server waits for the task to finish (or be destroyed) before it's destroyed: from now, rather than from when
    // the task starts, which could be after the server was destroyed (its destructor finding nothing to wait for).
    return accept_connections(running.acquire());
}

coro::task<> server::accept_connections(coro::async_scope::lease held)
{
    // the server's being destroyed: serve() was called too late.
    if (!held) co_return;

    // closed before it started.
    if (closed.load(std::memory_order::acquire)) co_return;

    if (serving.exchange(true, std::memory_order::acq_rel))
    {
        throw exception{"already serving"};
    }

    auto backoff = io::clock::duration::zero();

    while (!closed.load(std::memory_order::acquire))
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
    while (max_connections != 0 && !closed.load(std::memory_order::acquire))
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
    connection_state(detail::connection* conn, std::stop_token closing, std::optional<io::clock::duration> read_timeout)
        : writer{&detail::as_writer(*conn), buffer_size}
        , input{&detail::as_reader(*conn), &writer}
        , reader{&input, buffer_size}
        , conn{conn}
        , read_timeout{read_timeout}
        , closing{std::move(closing)}
        , on_close{std::in_place, this->closing, close_now{this}}
    {}

    // phase is where a connection is between requests, for close(): whose stop callback (close_now) and the connection
    // itself each move it, so that one of them sees the other's move.
    enum class phase : std::uint8_t
    {
        idle,    // waiting for a request's head (or HTTP/2's preface): close() shuts the connection's reading side.
        busy,    // serving a request (or handed over to HTTP/2): close() leaves it to finish it.
        closing, // close() was called: the connection ends, as soon as it isn't busy.
    };

    // close_now is close()'s callback: it shuts the connection's reading side (so reads find the end of the stream) if
    // it's idle, or leaves it to close once its request is done.
    struct close_now
    {
        connection_state* state;

        void operator()() const noexcept
        {
            state->closes.store(true, std::memory_order::relaxed);
            if (state->current.exchange(phase::closing, std::memory_order::acq_rel) == phase::idle)
                detail::socket_of(*state->conn).shutdown_read();
        }
    };

    // abort_now is abort()'s callback: it stops its handlers' token, and shuts the connection's reading side, whatever
    // it's doing.
    struct abort_now
    {
        connection_state* state;

        void operator()() const noexcept
        {
            // NOTE: the response says the connection closes (whether or not close()'s callback has come yet), before
            //       the token's stopped, so a handler stopped by it (responding, e.g. 503) sees that (relaxed: the stop
            //       token's request and look at it order them).
            // NOTE: the token before the reading side, so a handler reading (e.g. a WebSocket's) finds why it ended
            //       (canceled) before it finds the end of the stream.
            state->closes.store(true, std::memory_order::relaxed);
            state->stop.request_stop();
            detail::socket_of(*state->conn).shutdown_read();
        }
    };

    // serve_http1 readies the connection to serve HTTP/1.x requests (once it isn't HTTP/2): its handlers' stop source,
    // which abort() stops (as does the connection ending, or the client leaving: see the watch, below), abort()'s
    // callback, and the watcher.
    // NOTE: not before, as close() (which abort() calls too) ends a connection waiting for HTTP/2's preface anyway,
    //       and an HTTP/2 connection needs no source of its own.
    void serve_http1(const std::stop_token& aborting)
    {
        stop = std::stop_source{};
        on_abort.emplace(aborting, abort_now{this});

        watcher        = watch(this);
        watcher_parked = watcher.get_handle();
    }

    // The watch: while a handler waits, the connection is read, so that the client leaving (the end of the stream, a
    // reset, or TLS's close_notify) stops the handler's token. A coroutine of the connection's own (the watcher) reads
    // once per watch, into reader's buffer, and parks in between. A request is watched once its handler has suspended
    // (one that runs straight through isn't: watching it would only delay its response) and its body has been read to
    // its end (at once, for one without), with nothing buffered after it (a pipelined request shows the client was
    // there). Meanwhile the connection's input is held (see io::flush_before_read::hold()), as the handler may be
    // writing. Data ends the watch, and stays buffered for the next request. Once the handler's done, the server takes
    // the watch's read over as its wait for the next request's head (sending the response, and setting the head's
    // deadline on the read already waiting), so nothing's allocated per request, and no syscall's added.

    // watch is the watcher: a read per watch, after which it parks (see watch_done).
    static coro::task<> watch(connection_state* state)
    {
        while (true)
        {
            std::error_code err;
            try
            {
                err = co_await state->reader.fill();
            }
            catch (const std::bad_alloc&)
            {
                err = std::make_error_code(std::errc::not_enough_memory);
            }

            // the client's gone, or the connection's failed. (A timeout is the server's deadline, once the handler's
            // done, or abandon_watch().)
            if (err && err != std::errc::timed_out) state->stop.request_stop();

            co_await watch_done{.state = state, .err = err};
        }
    }

    // watch_done publishes a watch's result, continuing the server if it's waiting for it, and parks the watcher.
    struct watch_done
    {
        connection_state* state = nullptr;
        std::error_code   err;

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> watcher) const noexcept
        {
            state->watcher_parked = watcher;
            state->watch_result   = err;

            // NOTE: once it's published, the server may start the next watch at once, on another thread: nothing here
            //       may be touched after.
            void* waiting = state->watch_handoff.exchange(state, std::memory_order::acq_rel);
            if (waiting != nullptr) return std::coroutine_handle<>::from_address(waiting);
            return std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    // watch_ended waits for the watch's result.
    struct watch_ended
    {
        connection_state* state = nullptr;

        [[nodiscard]] bool await_ready() const noexcept
        {
            return state->watch_handoff.load(std::memory_order::acquire) == state;
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> server) const noexcept
        {
            void* none = nullptr;
            return state->watch_handoff.compare_exchange_strong(none,
                                                                server.address(),
                                                                std::memory_order::acq_rel,
                                                                std::memory_order::acquire);
        }

        [[nodiscard]] std::error_code await_resume() const noexcept { return state->watch_result; }
    };

    // The marks a watch waits for: whichever comes second (on whichever thread, as the handler may be running on
    // another by then) begins it. And a third, the handler switching protocols, after which none does (see
    // switch_now()).
    static constexpr std::uint8_t handler_waited = 1;
    static constexpr std::uint8_t body_read      = 2;
    static constexpr std::uint8_t switched       = 4;

    // gate_open marks one of them, returning whether it was the second.
    bool gate_open(std::uint8_t mark) noexcept
    {
        const auto before = watch_gate.fetch_or(mark, std::memory_order::acq_rel);
        return (before & mark) == 0 && (before | mark) == (handler_waited | body_read);
    }

    // body_ended is the request body's end_callback.
    static void body_ended(void* context) noexcept
    {
        auto* state = static_cast<connection_state*>(context);
        if (state->gate_open(body_read)) state->begin_watch();
    }

    // handler_runs awaits the handler's task, as task's awaiter does, marking the gate if the handler suspends.
    struct handler_runs
    {
        connection_state*                          state = nullptr;
        std::coroutine_handle<coro::promise<void>> handler;

        [[nodiscard]] bool await_ready() const noexcept { return handler.done(); }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> server) const noexcept
        {
            auto& promise = handler.promise();
            promise.set_continuation(server);
            handler.resume();
            if (promise.finished()) return false;

            // NOTE: the handler may be running on another thread already, but it doesn't touch what a watch does.
            if (state->gate_open(handler_waited)) state->begin_watch();

            return !promise.mark_awaiter_or_finished();
        }

        void await_resume() const { handler.promise().result(); }
    };

    // begin_watch begins a watch, unless there's no need (see the watch, above).
    void begin_watch() noexcept
    {
        if (reader.size() != 0 || stop.stop_requested())
        {
            no_watch();
            return;
        }

        watching = true;
        input.hold(true);

        // the handler may take as long as it likes: the server's deadline applies once it's done.
        auto& sock = detail::socket_of(*conn);
        sock.set_read_timeout(std::nullopt);

        // on the pool, as the watch's first read (and its wait) would otherwise delay the handler.
        auto* pool = sock.get_scheduler()->pool();
        if (auto watcher = std::exchange(watcher_parked, nullptr); !pool->resume(watcher)) watcher.resume();
    }

    // no_watch publishes that the gate opened, but begin_watch() found no need for a watch, for a switch that may be
    // waiting to know (see switch_now()), as a watch's end would be.
    void no_watch() noexcept
    {
        void* waiting = watch_handoff.exchange(this, std::memory_order::acq_rel);
        if (waiting == nullptr) return;

        // on the pool, as this may be the server's thread, in the middle of starting the handler (see handler_runs).
        auto  switching = std::coroutine_handle<>::from_address(waiting);
        auto* pool      = detail::socket_of(*conn).get_scheduler()->pool();
        if (!pool->resume(switching)) switching.resume();
    }

    // after_watch puts the connection back as it was before the watch, once its result's been taken.
    void after_watch() noexcept
    {
        watching = false;
        input.hold(false);
        detail::socket_of(*conn).set_read_timeout(read_timeout);
    }

    // abandon_watch ends a watch the server won't take over (the connection's ending): at once, as its read times out.
    coro::task<> abandon_watch()
    {
        if (!watching) return coro::ready_task<>();

        detail::socket_of(*conn).set_read_deadline(io::clock::now());
        return [](connection_state* state) -> coro::task<>
        {
            (void)co_await watch_ended{.state = state};
            state->after_watch();
        }(this);
    }

    // The switch: a handler may switch the connection to another protocol (see http/tunnel.hpp), whose bytes it then
    // has, as the tunnel, until it returns: the connection's reader and writer (so what the client sent along with its
    // request, and what a watch read, are kept), the input held, and no read timeout but the tunnel's own.

    // tunnel_bytes is the connection's tunnel.
    class tunnel_bytes final : public detail::tunnel
    {
    public:
        explicit tunnel_bytes(connection_state* state) noexcept
            : state{state}
        {}

        [[nodiscard]] io::buffered_reader& reader() noexcept override { return state->reader; }
        [[nodiscard]] io::buffered_writer& writer() noexcept override { return state->writer; }

        void set_read_timeout(std::optional<io::clock::duration> timeout) noexcept override
        {
            detail::socket_of(*state->conn).set_read_timeout(timeout);
        }

        void set_read_deadline(std::optional<io::clock::time_point> deadline) noexcept override
        {
            detail::socket_of(*state->conn).set_read_deadline(deadline);
        }

        // NOTE: over TLS, the socket's token is the stream's (see tls::stream::set_stop_token()).
        void set_stop_token(std::stop_token stop) noexcept override
        {
            detail::socket_of(*state->conn).set_stop_token(std::move(stop));
        }

        [[nodiscard]] std::stop_token closing() const noexcept override { return state->closing; }

        [[nodiscard]] io::scheduler* get_scheduler() const noexcept override
        {
            return detail::socket_of(*state->conn).get_scheduler();
        }

        void abort() noexcept override
        {
            aborted.store(true, std::memory_order::release);
            detail::socket_of(*state->conn).shutdown_read();
        }

        // was_aborted is whether abort() was called: then what's written but not flushed isn't sent.
        [[nodiscard]] bool was_aborted() const noexcept { return aborted.load(std::memory_order::acquire); }

    private:
        connection_state* state;
        std::atomic_bool  aborted = false;
    };

    // switch_now is the connection's switch_hook (see detail::switch_hook), on the handler's task.
    static coro::task<detail::tunnel*> switch_now(void* context)
    {
        auto* state = static_cast<connection_state*>(context);

        // until the request's body has been read to its end (on the handler's task, or for none, before it starts: so,
        // before this), what follows its head is the body.
        if ((state->watch_gate.load(std::memory_order::acquire) & body_read) == 0)
            return coro::ready_task<detail::tunnel*>(nullptr);

        // From now, no watch begins. If the gate had opened, whoever opened it (maybe the server's thread, as this
        // runs) began one, or found no need: which is waited for.
        if ((state->watch_gate.fetch_or(switched, std::memory_order::acq_rel) & handler_waited) != 0)
            return state->switch_after_watch();

        return coro::ready_task<detail::tunnel*>(state->switch_input());
    }

    // switch_after_watch is switch_now(), once the gate has opened: a watch in progress is abandoned, as the server's
    // own paths do (its read timing out at once), keeping what its read buffered.
    coro::task<detail::tunnel*> switch_after_watch()
    {
        detail::socket_of(*conn).set_read_deadline(io::clock::now());

        // NOTE: watching is read only once the watch is known to have ended (or not begun), as it's set as it begins.
        (void)co_await watch_ended{.state = this};
        if (watching) after_watch();

        co_return switch_input();
    }

    // switch_input makes the connection's input the tunnel's: reads no longer flush what's written (the tunnel's user
    // does), nor have the request body's timeout, or a deadline. Writes keep their timeout.
    detail::tunnel* switch_input() noexcept
    {
        input.hold(true);

        auto& sock = detail::socket_of(*conn);
        sock.set_read_deadline(std::nullopt);
        sock.set_read_timeout(std::nullopt);
        return &tunnel;
    }

    // start_request is whether to serve the request whose head (or HTTP/2's preface) was just read: not if close()
    // ended the connection as it waited for it (e.g. having read it from what was buffered, or before the reading side
    // was shut), as its body may then be cut short.
    bool start_request() noexcept
    {
        auto expected = phase::idle;
        return current.compare_exchange_strong(expected, phase::busy, std::memory_order::acq_rel);
    }

    // end_request is whether to wait for the next request, once one is done: not if the client, or the handler, asked
    // for the connection to close after the response (or the server's close() or abort() did: see response_writer's
    // close_after); nor if close() was called after the response's head was sent, which closes it all the same (as
    // abort() does, by shutting the reading side).
    bool end_request() noexcept
    {
        if (has_close(resp.headers)) return false;

        auto expected = phase::busy;
        return current.compare_exchange_strong(expected, phase::idle, std::memory_order::acq_rel);
    }

    // hand_over is whether to serve the connection as HTTP/2, once its preface was read: unless close() came first. Its
    // callback is unregistered (waiting for a call in progress), as HTTP/2 registers its own on closing, which ends it
    // gracefully.
    bool hand_over() noexcept
    {
        if (!start_request()) return false;

        on_close.reset();
        return true;
    }

    io::buffered_writer   writer;
    io::flush_before_read input;
    io::buffered_reader   reader;

    detail::connection* conn;

    // the socket's read timeout (body_read_timeout), which a watch's read goes without.
    std::optional<io::clock::duration> read_timeout;

    // the server's close(); and whether it or abort() has come, which responses check, to say the connection closes
    // after them (see response_writer's close_after).
    std::stop_token    closing;
    std::atomic<phase> current = phase::idle;
    std::atomic_bool   closes  = false;

    // the source of the handlers' tokens (server_request::stop): one for the connection's requests, as a request can
    // only be canceled by the connection ending. None until serve_http1().
    std::stop_source stop{std::nostopstate};

    // NOTE: after what they use, as they may run straight away (if the server's closing already), and are unregistered
    //       (waiting for a call in progress) before it's destroyed.
    std::optional<std::stop_callback<close_now>> on_close;
    std::optional<std::stop_callback<abort_now>> on_abort;

    server_request  req;
    server_response resp;

    // for reading what's left of a request's body, to drop it.
    std::array<std::byte, 4'096> scratch{};

    // The watch's (see above). Only the gate and the handoff are shared: the rest is touched by whoever begins a watch
    // (while the server waits for the handler), the watcher (until it publishes its result), and then the server.
    bool                      watching = false; // a watch was begun for the request, and not yet ended
    std::atomic<std::uint8_t> watch_gate{0};    // see gate_open()
    std::coroutine_handle<>   watcher_parked;   // the watcher, between watches
    std::error_code           watch_result;     // the last watch's, once watch_handoff says it's ended

    // watch_handoff is null while a watch's read is in progress (and from the start of each request, until a watch has
    // ended, or not begun: see no_watch()), this state once it's ended, or the server's coroutine (or a switch's) if
    // it's waiting for that.
    std::atomic<void*> watch_handoff{nullptr};

    // The switch's (see above): made once, for each request's response_writer to point to.
    tunnel_bytes        tunnel{this};
    detail::switch_hook switching{.call = &connection_state::switch_now, .context = this};

    // NOTE: last, so it's destroyed first (parked: the watch is always ended first), as it uses the rest.
    coro::task<> watcher;
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
            // TLS chooses HTTP/2 with ALPN; over cleartext, serve_requests() looks for its preface, and returns what it
            // read once its HTTP/1.x state is gone.
            // NOTE: serve_requests()'s frame (which holds that state) must be its own, freed as it returns, rather than
            //       elided into this one: Clang 23 doesn't elide it while its result's used (-Rpass-missed=coro-elide).
            if (http2_enabled && detail::alpn(*opened) == "h2"sv) co_await serve_http2(*opened, {});
            else if (auto prefaced = co_await serve_requests(*opened))
                co_await serve_http2(*opened, std::move(*prefaced));
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
    // NOTE: the handshake needs a deadline: without header_read_timeout, only body_read_timeout and write_timeout (set
    //       on the socket above: each wait for the client, while it makes no progress) and close() end one that
    //       doesn't finish.
    const auto deadline = head_deadline().value_or(io::clock::now() + std::chrono::hours{24} * 365);

    // the server decides what it speaks, whatever the context's options say.
    std::vector<std::string> protocols{"http/1.1"};
    if (http2_enabled) protocols.insert(protocols.begin(), "h2");

    // the answer to the client's close_notify is held back (as TLS 1.3 allows), so that an HTTP/1.1 response can
    // still be sent after it (see connection_state's watch).
    auto stream = co_await tls::stream::accept(std::move(conn),
                                               std::move(ctx),
                                               deadline,
                                               {
                                                   .stop             = closing.get_token(),
                                                   .alpn             = std::move(protocols),
                                                   .hold_close_reply = true,
                                               });
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

    // HTTP/2 answers it as it's read, as the stream does by default.
    if (stream->alpn() == "h2"sv) stream->release_close_reply();

    co_return std::optional<detail::connection>{std::in_place, std::move(*stream)};
}
#endif

// NOTE: each request is served here, rather than by coroutines of its own (see below), so it's long.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
coro::task<std::optional<std::vector<std::byte>>> server::serve_requests(detail::connection& conn)
{
    // close() ends the connection gracefully: at once if it's waiting for a request, or after the request it's serving
    // (see connection_state::phase). abort() (and the destructor) ends it either way.
    connection_state state{&conn, closing.get_token(), read_timeout()};
    auto&            req  = state.req;
    auto&            resp = state.resp;

    req.tls_version = detail::tls_version(conn);

    // NOTE: made only if the client sent a certificate, which it does only to a server that asks for one.
    req.peer_certificates = detail::peer_certificates(conn);

    // the server's, for the connection's requests (which reuse req).
    req.aborted = aborting.get_token();

    // the first request's head has header_read_timeout from now, whether or not it's looked at first for HTTP/2's
    // preface (once, rather than again after it); the next requests' from when they're waited for.
    auto deadline = head_deadline();

    // a cleartext client may start with HTTP/2's preface, knowing the server speaks it (RFC 9113, section 3.3).
    if (http2_enabled && req.tls_version == tls::version::none)
    {
        detail::socket_of(conn).set_read_deadline(deadline);
        if (co_await starts_with_preface(state.reader))
        {
            // HTTP/2 is served once this returns (see serve_connection()), with what was read: so the connection
            // doesn't keep its HTTP/1.x state (its buffers, and this frame) for its life. Unless close() came first.
            if (!state.hand_over()) co_return std::nullopt;
            const auto read = state.reader.buffered();
            co_return std::optional<std::vector<std::byte>>{std::in_place, read.begin(), read.end()};
        }
    }

    // abort() (or the destructor) ends the connection whatever it's doing, and stops its handlers' token, as does its
    // end, for a token a handler kept (e.g. in a task it spawned).
    state.serve_http1(aborting.get_token());
    const util::defer stop_at_end{[&state] noexcept { state.stop.request_stop(); }};

    // NOTE: each request is served here, rather than by a coroutine of its own, which would allocate its frame for
    //       each request (as one this large isn't elided). Only the rare ways a connection ends have their own.
    // NOTE: not conn.valid(), a syscall per request: a socket's pending error fails the next read or write anyway.
    while (true)
    {
        // Only the headers are read under the deadline: how long the handler takes to read the body is up to it.
        detail::socket_of(conn).set_read_deadline(deadline);

        auto err = co_await http11::request_decode(&state.reader, max_header_bytes, req);

        detail::socket_of(conn).set_read_deadline(std::nullopt);

        if (err)
        {
            co_await end_after_decode_error(conn, state, err);
            break;
        }

        // (filled before the body's end is called, so before a watch begins.)
        http11::keep_body_trailers(req);

        // (for a request without a body, at once.)
        state.watch_gate.store(0, std::memory_order::relaxed);
        state.watch_handoff.store(nullptr, std::memory_order::relaxed);
        http11::call_at_body_end(req, {.call = &connection_state::body_ended, .context = &state});

        if (!state.start_request())
        {
            logger->debug("closing connection: the server was closed as it waited for this request");
            break;
        }

        logger->trace("request {} {} as HTTP/{}.{}",
                      method_string(req.method),
                      req.target.path,
                      req.version.major,
                      req.version.minor);

        // a request line may claim any version (e.g. 2.0, or 3.0), but only HTTP/1.x is served this way: HTTP/2 is
        // chosen before any request is read (by ALPN, or its preface).
        if (req.version.major != 1)
        {
            logger->trace("unsupported http version: {}.{}", req.version.major, req.version.minor);
            co_await respond_and_close(conn, state, status::HTTP_VERSION_NOT_SUPPORTED);
            break;
        }

        // an expectation the server can't meet is refused (RFC 9110, section 10.1.1, says it MAY be, as Go's server
        // does), closing the connection, as what follows may be the body the client meant to send only if it was met.
        const auto expected = detail::expectation_of(req);
        if (expected == detail::expectation::unknown)
        {
            logger->debug("closing connection: the request has an expectation that can't be met");
            co_await respond_and_close(conn, state, status::EXPECTATION_FAILED);
            break;
        }

        io::discard_writer no_body;
        start_response(state, no_body);

        response_writer rw{&state.writer, &resp, http11::response_encode, &state.closes, &state.switching};

        // the client may wait for 100 Continue before it sends the body: it's sent when the handler first reads it.
        if (expected == detail::expectation::continue_100 && body_to_come(req))
            http11::call_at_body_start(req, rw.expect_continue());

        // NOTE: set for each request, as middleware may have replaced it for the handler it wrapped.
        req.stop = state.stop.get_token();

        // NOTE: a co_await can't be in a catch block, so a failure is responded to after it.
        std::exception_ptr failure;
        try
        {
            // the connection's watched while the handler waits (see connection_state's watch).
            auto handling = handler(req, rw);
            if (auto running = handling.get_handle())
                co_await connection_state::handler_runs{.state = &state, .handler = running};
            else co_await std::move(handling);
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        // (discarding the body doesn't begin a watch.)
        state.watch_gate.store(0, std::memory_order::relaxed);

        // NOTE: nothing below reads the connection while a watch is in progress: a watched request's body has been read
        //       to its end, so discarding the rest of it reads nothing.
        if (failure)
        {
            co_await state.abandon_watch();
            co_await end_after_handler_failure(conn, state, rw, failure);
            break;
        }

        // the handler switched protocols, and had the connection until it returned (see the switch, above; no watch is
        // in progress): the connection ends, sending what the handler left written, unless it aborted it.
        if (rw.switched())
        {
            if (!state.tunnel.was_aborted()) co_await state.writer.flush();
            co_return std::nullopt;
        }

        if (!co_await default_response(state, rw))
        {
            co_await state.abandon_watch();
            break;
        }

        // a response to HEAD has no body, so nothing after it either.
        if (req.method == request_method::HEAD) resp.trailers.clear();

        // ends a body sent without a Content-Length, chunked, with its trailers. If they can't be sent, the connection
        // closes without the last chunk, so the client doesn't take the response for complete.
        if (auto finished = co_await rw.finish(); finished.err)
        {
            if (finished.err == std::errc::invalid_argument)
            {
                logger->warn("handler for {} {} set a trailer that can't be sent",
                             method_string(req.method),
                             req.target.path);
            }
            else
            {
                logger->debug("failed to end the response's body: {}", finished.err.message());
            }

            co_await state.abandon_watch();
            break;
        }

        // the client would wait for the rest of the body, so it's told there's no more by closing the connection.
        if (wrote_less(req, rw))
        {
            logger->warn("handler for {} {} wrote {} bytes less than its Content-Length",
                         method_string(req.method),
                         req.target.path,
                         rw.body_remaining());
            co_await state.abandon_watch();
            break;
        }

        // NOTE: the response is sent when the server next reads from the connection (see io::flush_before_read), or
        //       as the watch ends (below).

        // the client waits for a 100 Continue it didn't get, so the body the handler didn't read may never come: rather
        // than waiting for it, the connection closes (as the response said), lingering, in case it's sent after all.
        // NOTE: a request whose body's unread isn't watched, so there's no watch in progress.
        if (rw.continue_unmet())
        {
            logger->debug("closing connection rather than asking for the rest of the request body");
            co_await state.writer.flush();
            co_await linger_before_close(conn);
            break;
        }

        // Whatever of the body the handler didn't read must be skipped, or it would be parsed as the next request.
        if (!co_await discard_body(*req.body, state.scratch))
        {
            logger->debug("closing connection rather than discarding the rest of the request body");
            co_await state.abandon_watch();
            break;
        }

        if (!state.end_request())
        {
            co_await state.abandon_watch();
            break;
        }

        // the next request's head, from now.
        deadline = head_deadline();

        // The watch's read becomes the wait for the next request's head: the response is sent now (as the watch held
        // it: see io::flush_before_read), and the head's deadline (and the read timeout, which the watch's read went
        // without) applies to the read already waiting.
        if (state.watching)
        {
            const auto flushed = co_await state.writer.flush();

            const auto now   = io::clock::now();
            auto       until = deadline;
            if (const auto timeout = read_timeout()) until = until ? std::min(*until, now + *timeout) : now + *timeout;
            detail::socket_of(conn).set_read_deadline(flushed.err ? now : until);

            const auto watched = co_await connection_state::watch_ended{.state = &state};
            state.after_watch();

            if (flushed.err)
            {
                logger->debug("failed to send the response: {}", flushed.err.message());
                break;
            }

            // e.g. the client closed the connection, or its next request's head didn't come in time.
            if (watched)
            {
                co_await end_after_decode_error(conn, state, watched);
                break;
            }
        }
    }

    co_await state.writer.flush();
    co_return std::nullopt;
}

coro::task<> server::serve_http2(detail::connection& conn, std::vector<std::byte> already_read)
{
    // HTTP/2's reader waits for as long as the connection's open: its streams, and the connection, have timeouts of
    // their own.
    auto& sock = detail::socket_of(conn);
    sock.set_read_timeout(std::nullopt);
    sock.set_read_deadline(std::nullopt);

    co_await detail::serve_http2(scheduler,
                                 conn,
                                 std::move(already_read),
                                 http2_config,
                                 handler,
                                 *logger,
                                 closing.get_token(),
                                 aborting.get_token());
}

std::optional<io::clock::time_point> server::head_deadline() const
{
    if (header_read_timeout <= std::chrono::seconds::zero()) return std::nullopt;
    return io::clock::now() + header_read_timeout;
}

std::optional<io::clock::duration> server::read_timeout() const
{
    if (body_read_timeout <= std::chrono::seconds::zero()) return std::nullopt;
    return body_read_timeout;
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
    resp.trailers.clear();
    resp.headers.set("Date"sv, current_date());

    // HTTP/1.1 connections persist unless either end says they close, and HTTP/1.0's close unless the client says they
    // persist (RFC 9112, section 9.3): the response says which, if it isn't the default.
    const bool http10 = req.version.minor == 0;
    if (has_close(req.headers) || (http10 && !req.headers.has_token("Connection"sv, "keep-alive"sv)))
        resp.headers.set("Connection"sv, "close"sv);
    else if (http10) resp.headers.set("Connection"sv, "keep-alive"sv);
}

coro::task<bool> server::default_response(connection_state& state, response_writer& rw) const
{
    if (rw.sent()) return coro::ready_task<bool>(true);

    if (state.stop.stop_requested())
    {
        logger->debug("closing connection: the handler for {} {} was stopped without responding",
                      method_string(state.req.method),
                      state.req.target.path);
        return coro::ready_task<bool>(false);
    }

    // usually, the head fits in the connection's buffer, so there's no need for a coroutine (see coro::ready_task).
    auto sending = rw.send(status::OK, 0);
    if (!sending.finished_without_coroutine()) return sent_after(std::move(sending));

    (void)std::move(sending).result();
    return coro::ready_task<bool>(true);
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

    // a handler that throws once it's been stopped (e.g. a database driver's own "canceled") isn't an error of the
    // server's, and gets no 500, as no one wants its answer.
    const bool stopped = state.stop.stop_requested();
    const auto level   = stopped ? spdlog::level::debug : spdlog::level::err;

    try
    {
        std::rethrow_exception(failure);
    }
    catch (const std::exception& ex)
    {
        logger->log(level, "handler for {} {} threw: {}", method_string(req.method), req.target.path, ex.what());
    }
    catch (...)
    {
        logger->log(level, "handler for {} {} threw", method_string(req.method), req.target.path);
    }

    // whatever the handler sent of the response can't be finished, so the connection is closed: after a 500, if it
    // sent nothing but interim responses.
    if (!rw.sent() && !stopped)
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
    resp.trailers.clear();
    resp.headers.set("Connection"sv, "close"sv);
    resp.headers.set("Date"sv, current_date());

    response_writer rw{&state.writer, &resp, http11::response_encode};
    co_await rw.send(code, 0);
    co_await state.writer.flush();
    co_await linger_before_close(conn);
}

}
